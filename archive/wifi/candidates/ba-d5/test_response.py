"""Differentially execute actual pre/post-change ADDBA response functions."""
from pathlib import Path
import re, subprocess
p=Path(__file__).resolve().parent
roots=[p.parent/'native-rx-fix/source',p/'source']
def extract(text,name):
 m=re.search(r'\n'+name+r'\([^;]+?\)\n\{',text,re.S);assert m,name
 at=m.end();depth=1
 while depth:
  depth+=(text[at]=='{')-(text[at]=='}');at+=1
 return 'void '+text[m.start()+1:at]
functions=['ieee80211_recv_addba_resp','ieee80211_addba_resp_accept','ieee80211_addba_resp_refuse']
# Constants are extracted from the actual headers, not independently invented.
allh='\n'.join((roots[1]/'rtw80211/openbsd/net80211'/x).read_text() for x in ['ieee80211.h','ieee80211_node.h'])
constants={}
for name in ['IEEE80211_BA_INIT','IEEE80211_BA_REQUESTED','IEEE80211_BA_AGREED','IEEE80211_STATUS_SUCCESS','IEEE80211_STATUS_UNSPECIFIED','IEEE80211_REASON_SETUP_REQUIRED','IEEE80211_ACTION_DELBA','IEEE80211_ADDBA_REQ_INTVAL_MAX','IEEE80211_CATEG_BA']:
 m=re.search(r'(?:#define\s+|\b)'+name+r'\s*(?:=\s*)?(0x[0-9a-fA-F]+|\d+)\b',allh);assert m,name
 constants[name]=int(m[1],0)
src=r'''
#include <cassert>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include "source/rtw80211/openbsd/net80211/ieee80211_ba_diag.h"
#define DPRINTF(...) ((void)0)
#define mtod(m,type) reinterpret_cast<type>((m)->data)
#define LE_READ_2(p) (unsigned((p)[0]) | (unsigned((p)[1])<<8))
struct Packet {unsigned length; uint8_t data[64];};
using mbuf_t=Packet*;
unsigned mbuf_len(mbuf_t m){return m->length;}
struct ieee80211_frame {uint8_t data[24];};
struct ieee80211_node;
struct ieee80211com {
 uint64_t ic_wtl_ba_diag[WTL_BA_COUNT]{};
 struct {unsigned is_ht_tx_ba_agreements{};} ic_stats;
 int (*ic_ampdu_tx_start)(ieee80211com*,ieee80211_node*,uint8_t){};
 int hw_result{},hw_calls{},actions{},timer_calls{};
};
struct Timer {bool pending{true};};
struct ieee80211_tx_ba {
 ieee80211_node* ba_ni{};Timer timer{},*ba_to{&timer};
 int ba_state{},ba_timeout_val{};uint8_t ba_token{};
 uint16_t ba_winstart{},ba_winsize{};
};
struct ieee80211_node {
 ieee80211_tx_ba ni_tx_ba[16]{};
 unsigned ni_addba_req_intval[16]{};uint16_t ni_qos_txseqs[16]{};
};
void timeout_del(Timer** t){(*t)->pending=false;}
void timeout_add_usec(Timer** t,int){(*t)->pending=true;}
int hardware(ieee80211com*ic,ieee80211_node*,uint8_t){++ic->hw_calls;return ic->hw_result;}
#define IEEE80211_SEND_ACTION(ic,ni,cat,action,arg) (++(ic)->actions)
'''
src+=''.join(f'constexpr int {k}={v};\n' for k,v in constants.items())
for i,root in enumerate(roots):
 txt=(root/'rtw80211/openbsd/net80211/ieee80211_input.c').read_text()
 src+=f'namespace variant{i} {{\n'
 src+='void ieee80211_addba_resp_accept(ieee80211com*,ieee80211_node*,uint8_t);\nvoid ieee80211_addba_resp_refuse(ieee80211com*,ieee80211_node*,uint8_t,uint16_t);\n'
 src+='\n'.join(extract(txt,name) for name in functions)+'\n}\n'
src+=r'''
int main(){
 unsigned count=0;
 for(int tid=0;tid<16;++tid)for(int state=0;state<3;++state)
 for(int token=0;token<2;++token)for(int status=0;status<2;++status)
 for(int hw: {0,EIO,EBUSY})for(int shortFrame=0;shortFrame<2;++shortFrame){
  ieee80211com a{},b{};ieee80211_node na{},nb{};Packet packet{};
  a.ic_ampdu_tx_start=b.ic_ampdu_tx_start=hardware;a.hw_result=b.hw_result=hw;
  auto&x=na.ni_tx_ba[tid];auto&y=nb.ni_tx_ba[tid];
  x.ba_state=y.ba_state=state;x.ba_token=y.ba_token=77;
  x.ba_winstart=y.ba_winstart=4095;x.ba_timeout_val=y.ba_timeout_val=10;
  packet.length=shortFrame?32:33;packet.data[26]=token?77:78;
  packet.data[27]=status?37:0;packet.data[29]=uint8_t((tid<<2)|2);packet.data[30]=16;
  variant0::ieee80211_recv_addba_resp(&a,&packet,&na);
  variant1::ieee80211_recv_addba_resp(&b,&packet,&nb);
  assert(x.ba_state==y.ba_state&&x.timer.pending==y.timer.pending);
  assert(na.ni_qos_txseqs[tid]==nb.ni_qos_txseqs[tid]);
  assert(na.ni_addba_req_intval[tid]==nb.ni_addba_req_intval[tid]);
  assert(a.hw_calls==b.hw_calls&&a.actions==b.actions);
  assert(a.ic_stats.is_ht_tx_ba_agreements==b.ic_stats.is_ht_tx_ba_agreements);
  assert(b.ic_wtl_ba_diag[WTL_BA_TxResponse]==1);
  if(shortFrame)assert(b.ic_wtl_ba_diag[WTL_BA_TxResponseShort]==1&&b.hw_calls==0);
  else if(state!=1)assert(b.ic_wtl_ba_diag[WTL_BA_TxResponseNoRequest]==1&&b.hw_calls==0);
  else if(!token)assert(b.ic_wtl_ba_diag[WTL_BA_TxResponseTokenMismatch]==1&&b.hw_calls==0);
  else if(status)assert(b.ic_wtl_ba_diag[WTL_BA_TxResponseRefused]==1&&b.hw_calls==0);
  else if(hw==EIO)assert(b.ic_wtl_ba_diag[WTL_BA_TxHardwareFailure]==1);
  else if(hw==0)assert(b.ic_wtl_ba_diag[WTL_BA_TxAgreement]==1);
  ++count;
 }
 printf("PASS: %u actual-source differential ADDBA response cases; outcomes unchanged, stage counters accurate.\n",count);
}
'''
src=src.replace('#include <cstring>','#include <cstring>\n#include <initializer_list>')
(p/'response-under-test.cpp').write_text(src)
subprocess.run(['clang++','-std=c++17','-O1','-g','-fsanitize=address,undefined',str(p/'response-under-test.cpp'),'-o',str(p/'test-response')],check=True)
subprocess.run([str(p/'test-response')],check=True)
