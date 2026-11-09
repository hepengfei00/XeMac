
#include <cassert>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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
constexpr int IEEE80211_BA_INIT=0;
constexpr int IEEE80211_BA_REQUESTED=1;
constexpr int IEEE80211_BA_AGREED=2;
constexpr int IEEE80211_STATUS_SUCCESS=0;
constexpr int IEEE80211_STATUS_UNSPECIFIED=1;
constexpr int IEEE80211_REASON_SETUP_REQUIRED=38;
constexpr int IEEE80211_ACTION_DELBA=2;
constexpr int IEEE80211_ADDBA_REQ_INTVAL_MAX=30;
constexpr int IEEE80211_CATEG_BA=3;
namespace variant0 {
void ieee80211_addba_resp_accept(ieee80211com*,ieee80211_node*,uint8_t);
void ieee80211_addba_resp_refuse(ieee80211com*,ieee80211_node*,uint8_t,uint16_t);
void ieee80211_recv_addba_resp(struct ieee80211com *ic, mbuf_t m,
                          struct ieee80211_node *ni)
{
    const struct ieee80211_frame *wh;
    const u_int8_t *frm;
    struct ieee80211_tx_ba *ba;
    u_int16_t status, params, bufsz, timeout;
    u_int8_t token, tid;
    int err = 0;
    
    if (mbuf_len(m) < sizeof(*wh) + 9) {
        DPRINTF(("frame too short\n"));
        return;
    }
    wh = mtod(m, struct ieee80211_frame *);
    frm = (const u_int8_t *)&wh[1];
    
    token = frm[2];
    status = LE_READ_2(&frm[3]);
    params = LE_READ_2(&frm[5]);
    tid = (params >> 2) & 0xf;
    bufsz = (params >> 6) & 0x3ff;
    timeout = LE_READ_2(&frm[7]);
    
    DPRINTF(("received ADDBA resp from %s, TID %d, status %d\n",
             ether_sprintf(ni->ni_macaddr), tid, status));
    
    /*
     * Ignore if no ADDBA request has been sent for this RA/TID or
     * if we already have a Block Ack agreement.
     */
    ba = &ni->ni_tx_ba[tid];
    if (ba->ba_state != IEEE80211_BA_REQUESTED) {
        DPRINTF(("no matching ADDBA req found\n"));
        return;
    }
    if (token != ba->ba_token) {
        DPRINTF(("ignoring ADDBA resp from %s: token %x!=%x\n",
                 ether_sprintf(ni->ni_macaddr), token, ba->ba_token));
        return;
    }
    /* we got an ADDBA Response matching our request, stop timeout */
    timeout_del(&ba->ba_to);
    
    if (status != IEEE80211_STATUS_SUCCESS) {
        if (ni->ni_addba_req_intval[tid] <
            IEEE80211_ADDBA_REQ_INTVAL_MAX)
            ni->ni_addba_req_intval[tid]++;
        
        ieee80211_addba_resp_refuse(ic, ni, tid, status);
        
        /*
         * In case the peer believes there is an existing
         * block ack agreement with us, try to delete it.
         */
        IEEE80211_SEND_ACTION(ic, ni, IEEE80211_CATEG_BA,
                              IEEE80211_ACTION_DELBA,
                              IEEE80211_REASON_SETUP_REQUIRED << 16 | 1 << 8 | tid);
        return;
    }
    
    /* notify drivers of this new Block Ack agreement */
    if (ic->ic_ampdu_tx_start != NULL)
        err = ic->ic_ampdu_tx_start(ic, ni, tid);
    
    if (err == EBUSY) {
        /* driver will accept or refuse agreement when done */
        return;
    } else if (err) {
        /* driver failed to setup, rollback */
        ieee80211_addba_resp_refuse(ic, ni, tid,
                                    IEEE80211_STATUS_UNSPECIFIED);
    } else
        ieee80211_addba_resp_accept(ic, ni, tid);
}
void ieee80211_addba_resp_accept(struct ieee80211com *ic,
                            struct ieee80211_node *ni, uint8_t tid)
{
    struct ieee80211_tx_ba *ba = &ni->ni_tx_ba[tid];
    
    /* MLME-ADDBA.confirm(Success) */
    ba->ba_state = IEEE80211_BA_AGREED;
    ic->ic_stats.is_ht_tx_ba_agreements++;
    
    ni->ni_qos_txseqs[tid] = ba->ba_winstart;
    
    /* Reset ADDBA request interval. */
    ni->ni_addba_req_intval[tid] = 1;
    
    /* start Block Ack inactivity timeout */
    if (ba->ba_timeout_val != 0)
        timeout_add_usec(&ba->ba_to, ba->ba_timeout_val);
}
void ieee80211_addba_resp_refuse(struct ieee80211com *ic,
                            struct ieee80211_node *ni, uint8_t tid, uint16_t status)
{
    struct ieee80211_tx_ba *ba = &ni->ni_tx_ba[tid];
    
    /* MLME-ADDBA.confirm(Failure) */
    ba->ba_state = IEEE80211_BA_INIT;
}
}
namespace variant1 {
void ieee80211_addba_resp_accept(ieee80211com*,ieee80211_node*,uint8_t);
void ieee80211_addba_resp_refuse(ieee80211com*,ieee80211_node*,uint8_t,uint16_t);
void ieee80211_recv_addba_resp(struct ieee80211com *ic, mbuf_t m,
                          struct ieee80211_node *ni)
{
    const struct ieee80211_frame *wh;
    const u_int8_t *frm;
    struct ieee80211_tx_ba *ba;
    u_int16_t status, params, bufsz, timeout;
    u_int8_t token, tid;
    int err = 0;
    
    WTL_BA_INC(ic, TxResponse);
    if (mbuf_len(m) < sizeof(*wh) + 9) {
        WTL_BA_INC(ic, TxResponseShort);
        DPRINTF(("frame too short\n"));
        return;
    }
    wh = mtod(m, struct ieee80211_frame *);
    frm = (const u_int8_t *)&wh[1];
    
    token = frm[2];
    status = LE_READ_2(&frm[3]);
    ic->ic_wtl_ba_diag[WTL_BA_LastRxResponseStatus] = status;
    params = LE_READ_2(&frm[5]);
    tid = (params >> 2) & 0xf;
    bufsz = (params >> 6) & 0x3ff;
    timeout = LE_READ_2(&frm[7]);
    
    DPRINTF(("received ADDBA resp from %s, TID %d, status %d\n",
             ether_sprintf(ni->ni_macaddr), tid, status));
    
    /*
     * Ignore if no ADDBA request has been sent for this RA/TID or
     * if we already have a Block Ack agreement.
     */
    ba = &ni->ni_tx_ba[tid];
    if (ba->ba_state != IEEE80211_BA_REQUESTED) {
        WTL_BA_INC(ic, TxResponseNoRequest);
        DPRINTF(("no matching ADDBA req found\n"));
        return;
    }
    if (token != ba->ba_token) {
        WTL_BA_INC(ic, TxResponseTokenMismatch);
        DPRINTF(("ignoring ADDBA resp from %s: token %x!=%x\n",
                 ether_sprintf(ni->ni_macaddr), token, ba->ba_token));
        return;
    }
    /* we got an ADDBA Response matching our request, stop timeout */
    timeout_del(&ba->ba_to);
    
    if (status != IEEE80211_STATUS_SUCCESS) {
        WTL_BA_INC(ic, TxResponseRefused);
        if (ni->ni_addba_req_intval[tid] <
            IEEE80211_ADDBA_REQ_INTVAL_MAX)
            ni->ni_addba_req_intval[tid]++;
        
        ieee80211_addba_resp_refuse(ic, ni, tid, status);
        
        /*
         * In case the peer believes there is an existing
         * block ack agreement with us, try to delete it.
         */
        IEEE80211_SEND_ACTION(ic, ni, IEEE80211_CATEG_BA,
                              IEEE80211_ACTION_DELBA,
                              IEEE80211_REASON_SETUP_REQUIRED << 16 | 1 << 8 | tid);
        return;
    }
    
    /* notify drivers of this new Block Ack agreement */
    if (ic->ic_ampdu_tx_start != NULL)
        err = ic->ic_ampdu_tx_start(ic, ni, tid);
    
    if (err == EBUSY) {
        /* driver will accept or refuse agreement when done */
        return;
    } else if (err) {
        /* driver failed to setup, rollback */
        WTL_BA_INC(ic, TxHardwareFailure);
        ieee80211_addba_resp_refuse(ic, ni, tid,
                                    IEEE80211_STATUS_UNSPECIFIED);
    } else
        ieee80211_addba_resp_accept(ic, ni, tid);
}
void ieee80211_addba_resp_accept(struct ieee80211com *ic,
                            struct ieee80211_node *ni, uint8_t tid)
{
    struct ieee80211_tx_ba *ba = &ni->ni_tx_ba[tid];
    
    /* MLME-ADDBA.confirm(Success) */
    WTL_BA_INC(ic, TxAgreement);
    ba->ba_state = IEEE80211_BA_AGREED;
    ic->ic_stats.is_ht_tx_ba_agreements++;
    
    ni->ni_qos_txseqs[tid] = ba->ba_winstart;
    
    /* Reset ADDBA request interval. */
    ni->ni_addba_req_intval[tid] = 1;
    
    /* start Block Ack inactivity timeout */
    if (ba->ba_timeout_val != 0)
        timeout_add_usec(&ba->ba_to, ba->ba_timeout_val);
}
void ieee80211_addba_resp_refuse(struct ieee80211com *ic,
                            struct ieee80211_node *ni, uint8_t tid, uint16_t status)
{
    struct ieee80211_tx_ba *ba = &ni->ni_tx_ba[tid];
    
    /* MLME-ADDBA.confirm(Failure) */
    ba->ba_state = IEEE80211_BA_INIT;
}
}

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
