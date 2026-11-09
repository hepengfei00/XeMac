from pathlib import Path
root=Path(__file__).resolve().parent/'source'
fields=[
 'TxRequest','TxRequestQueued','TxRequestQueueError','TxResponseTimeout',
 'TxResponse','TxResponseShort','TxResponseNoRequest','TxResponseTokenMismatch',
 'TxResponseRefused','TxHardwareFailure','TxAgreement',
 'RxRequest','RxNotReady','RxNonHT','RxShort','RxBusy','RxRefused','RxHardwareFailure','RxAgreement',
 'RxActionValidated','RxAPAction','RxAPProtectedAction','RxUnprotectedMgmtDrop','RxMgmtDecryptFailure','RxProtectionOffDrop',
 'LastTxParams','LastRxParams','LastRxResponseStatus',
]
header='''/* Local passive BA diagnostics. Contains no addresses, payloads or keys.
 * All accesses on the d4 native protocol gate. Never bypass protocol checks. */
#ifndef WTL_IEEE80211_BA_DIAGNOSTICS_H
#define WTL_IEEE80211_BA_DIAGNOSTICS_H
'''
header+='enum ieee80211_wtl_ba_diag {\n'+''.join('    WTL_BA_'+x+',\n' for x in fields)+'    WTL_BA_COUNT\n};\n'
header+='#define WTL_BA_INC(ic, field) (++(ic)->ic_wtl_ba_diag[WTL_BA_##field])\n#endif\n'
(root/'rtw80211/openbsd/net80211/ieee80211_ba_diag.h').write_text(header)
def edit(rel,fn):
 p=root/rel; raw=p.read_bytes(); s=raw.decode().replace('\r\n','\n'); out=fn(s)
 assert out!=s, rel
 p.write_bytes(out.replace('\n','\r\n').encode() if b'\r\n' in raw else out.encode())
def rep(s,a,b):
 n=s.count(a)
 if n!=1:raise ValueError((a[:120],n))
 return s.replace(a,b,1)
def func(s,name,fn):
 import re
 m=re.search(r'\n'+name+r'\([^;]+?\)\n\{',s,re.S)
 if not m:raise ValueError(name)
 start=m.start(); at=m.end(); depth=1
 while depth:
  if s[at]=='{':depth+=1
  elif s[at]=='}':depth-=1
  at+=1
 return s[:start]+fn(s[start:at])+s[at:]
def var(s):
 s=rep(s,'#include <net80211/ieee80211_proto.h>','#include <net80211/ieee80211_proto.h>\n#include <net80211/ieee80211_ba_diag.h>')
 return rep(s,'struct ieee80211_stats\tic_stats;', 'uint64_t ic_wtl_ba_diag[WTL_BA_COUNT]; /* passive local counters */\n\tstruct ieee80211_stats\tic_stats;')
edit('rtw80211/openbsd/net80211/ieee80211_var.h',var)
def inp(s):
 def req(f):
  f=rep(f,'    /* Ignore if we are not ready', '    WTL_BA_INC(ic, RxRequest);\n    /* Ignore if we are not ready')
  f=rep(f,'((ic->ic_flags & IEEE80211_F_RSNON) && !ni->ni_port_valid))\n        return;', '((ic->ic_flags & IEEE80211_F_RSNON) && !ni->ni_port_valid)) {\n        WTL_BA_INC(ic, RxNotReady);\n        return;\n    }')
  f=rep(f,'if (!(ni->ni_flags & IEEE80211_NODE_HT)) {', 'if (!(ni->ni_flags & IEEE80211_NODE_HT)) {\n        WTL_BA_INC(ic, RxNonHT);')
  f=rep(f,'if (mbuf_len(m) < sizeof(*wh) + 9) {','if (mbuf_len(m) < sizeof(*wh) + 9) {\n        WTL_BA_INC(ic, RxShort);')
  f=rep(f,'params = LE_READ_2(&frm[3]);','params = LE_READ_2(&frm[3]);\n    ic->ic_wtl_ba_diag[WTL_BA_LastRxParams] = params;')
  f=rep(f,'if (ba->ba_state == IEEE80211_BA_REQUESTED)\n        return;', 'if (ba->ba_state == IEEE80211_BA_REQUESTED) {\n        WTL_BA_INC(ic, RxBusy);\n        return;\n    }')
  f=rep(f,'/* driver failed to setup, rollback */','/* driver failed to setup, rollback */\n        WTL_BA_INC(ic, RxHardwareFailure);')
  f=rep(f,'refuse:\n','refuse:\n    WTL_BA_INC(ic, RxRefused);\n')
  return f
 s=func(s,'ieee80211_recv_addba_req',req)
 s=func(s,'ieee80211_addba_req_accept',lambda f:rep(f,'ba->ba_state = IEEE80211_BA_AGREED;', 'WTL_BA_INC(ic, RxAgreement);\n    ba->ba_state = IEEE80211_BA_AGREED;'))
 s=func(s,'ieee80211_addba_req_refuse',lambda f:rep(f,'free(ba->ba_buf);','WTL_BA_INC(ic, RxRefused);\n    free(ba->ba_buf);'))
 def resp(f):
  f=rep(f,'if (mbuf_len(m) < sizeof(*wh) + 9) {','WTL_BA_INC(ic, TxResponse);\n    if (mbuf_len(m) < sizeof(*wh) + 9) {\n        WTL_BA_INC(ic, TxResponseShort);')
  f=rep(f,'status = LE_READ_2(&frm[3]);','status = LE_READ_2(&frm[3]);\n    ic->ic_wtl_ba_diag[WTL_BA_LastRxResponseStatus] = status;')
  f=rep(f,'if (ba->ba_state != IEEE80211_BA_REQUESTED) {','if (ba->ba_state != IEEE80211_BA_REQUESTED) {\n        WTL_BA_INC(ic, TxResponseNoRequest);')
  f=rep(f,'if (token != ba->ba_token) {','if (token != ba->ba_token) {\n        WTL_BA_INC(ic, TxResponseTokenMismatch);')
  f=rep(f,'if (status != IEEE80211_STATUS_SUCCESS) {','if (status != IEEE80211_STATUS_SUCCESS) {\n        WTL_BA_INC(ic, TxResponseRefused);')
  f=rep(f,'/* driver failed to setup, rollback */','/* driver failed to setup, rollback */\n        WTL_BA_INC(ic, TxHardwareFailure);')
  return f
 s=func(s,'ieee80211_recv_addba_resp',resp)
 s=func(s,'ieee80211_addba_resp_accept',lambda f:rep(f,'ba->ba_state = IEEE80211_BA_AGREED;', 'WTL_BA_INC(ic, TxAgreement);\n    ba->ba_state = IEEE80211_BA_AGREED;'))
 s=func(s,'ieee80211_recv_action',lambda f:rep(f,'switch (frm[0]) {','WTL_BA_INC(ic, RxActionValidated);\n    switch (frm[0]) {'))
 s=rep(s,'/* unicast mgmt not encrypted */\n                        goto out;', '/* unicast mgmt not encrypted */\n                        WTL_BA_INC(ic, RxUnprotectedMgmtDrop);\n                        goto out;')
 s=rep(s,'/* XXX stats */\n                        goto out;', '/* XXX stats */\n                        WTL_BA_INC(ic, RxMgmtDecryptFailure);\n                        goto out;')
 s=rep(s,'/* encrypted but MMPDU Rx protection off for TA */\n                goto out;', '/* encrypted but MMPDU Rx protection off for TA */\n                WTL_BA_INC(ic, RxProtectionOffDrop);\n                goto out;')
 return s
edit('rtw80211/openbsd/net80211/ieee80211_input.c',inp)
def proto(s):
 s=func(s,'ieee80211_tx_ba_timeout',lambda f:rep(f,'/* MLME-ADDBA.confirm(TIMEOUT) */','/* MLME-ADDBA.confirm(TIMEOUT) */\n        WTL_BA_INC(ic, TxResponseTimeout);'))
 def req(f):
  f=rep(f,'/* MLME-ADDBA.request */','WTL_BA_INC(ic, TxRequest);\n\t/* MLME-ADDBA.request */')
  f=rep(f,'    timeout_add_sec(&ba->ba_to, 1);', '    ic->ic_wtl_ba_diag[WTL_BA_LastTxParams] = ba->ba_params;\n    timeout_add_sec(&ba->ba_to, 1);')
  f=rep(f,'    IEEE80211_SEND_ACTION(ic, ni, IEEE80211_CATEG_BA,\n                          IEEE80211_ACTION_ADDBA_REQ, tid);', '    const int sendResult = IEEE80211_SEND_ACTION(ic, ni, IEEE80211_CATEG_BA,\n                          IEEE80211_ACTION_ADDBA_REQ, tid);\n    if (sendResult == 0) WTL_BA_INC(ic, TxRequestQueued);\n    else WTL_BA_INC(ic, TxRequestQueueError);')
  return f
 return func(s,'ieee80211_addba_request',req)
edit('rtw80211/openbsd/net80211/ieee80211_proto.c',proto)
def core(s):
 a='    target->setProperty("RTW89TxBackpressureCount", uint64_t(txBackpressureCount), 64);'
 b=a+'\n    // Passive Release diagnostics: publish protocol-validated BA state only.\n'
 b+='    static const char *const baNames[WTL_BA_COUNT] = {\n'+''.join('        "RTWBADiag'+x+'",\n' for x in fields)+'    };\n'
 b+='''    for (unsigned i = 0; i < WTL_BA_COUNT; ++i)
        target->setProperty(baNames[i], sc->sc_ic.ic_wtl_ba_diag[i], 64);
    target->setProperty("RTWBAProbeBuild", "2.3.1d5-passive-ba-stages");
    target->setProperty("RTWBAState", uint64_t(sc->sc_ic.ic_state), 32);
    target->setProperty("RTWBACaps", uint64_t(sc->sc_ic.ic_caps), 32);
    target->setProperty("RTWBAFlags", uint64_t(sc->sc_ic.ic_flags), 32);
    target->setProperty("RTWBAStationState", uint64_t(linuxContext.stationState), 32);
    auto *baNode = sc->sc_ic.ic_bss;
    target->setProperty("RTWBANodeFlags", uint64_t(baNode ? baNode->ni_flags : 0), 32);
    target->setProperty("RTWBAPortValid", bool(baNode && baNode->ni_port_valid));
    target->setProperty("RTWBAPairwiseSoftwareCrypto", bool(baNode &&
        (baNode->ni_pairwise_key.k_flags & IEEE80211_KEY_SWCRYPTO)));
    uint64_t txStates = 0, rxStates = 0;
    for (unsigned i = 0; baNode && i < IEEE80211_NUM_TID; ++i) {
        txStates |= uint64_t(baNode->ni_tx_ba[i].ba_state & 3) << (2*i);
        rxStates |= uint64_t(baNode->ni_rx_ba[i].ba_state & 3) << (2*i);
    }
    target->setProperty("RTWBATxStates", txStates, 64);
    target->setProperty("RTWBARxStates", rxStates, 64);
    target->setProperty("RTWBARequestPendingBE", bool(baNode &&
        timeout_pending(&baNode->ni_addba_req_to[EDCA_AC_BE])));
    target->setProperty("RTWBAActionTxCompleted", uint64_t(linuxContext.actionTxCount), 64);
    target->setProperty("RTWBAActionTxAcked", uint64_t(linuxContext.actionTxAckCount), 64);
    target->setProperty("RTWBATxHardwareStart", uint64_t(linuxContext.ampduTxStartCount), 64);
    target->setProperty("RTWBATxHardwareFailure", uint64_t(linuxContext.ampduTxStartFailCount), 64);
'''
 s=rep(s,a,b)
 a='    __sync_fetch_and_add(&owner->acceptedReceiveCount, 1);'
 b=a+'''
    if (frameType == IEEE80211_FC0_TYPE_MGT &&
        frameSubtype == IEEE80211_FC0_SUBTYPE_ACTION && node == ic->ic_bss &&
        IEEE80211_ADDR_EQ(wh->i_addr1, ic->ic_myaddr) &&
        IEEE80211_ADDR_EQ(wh->i_addr2, node->ni_bssid) &&
        IEEE80211_ADDR_EQ(wh->i_addr3, node->ni_bssid)) {
        WTL_BA_INC(ic, RxAPAction);
        if (wh->i_fc[1] & IEEE80211_FC1_PROTECTED)
            WTL_BA_INC(ic, RxAPProtectedAction);
    }
'''
 return rep(s,a,b)
edit('rtw/hal_rtw89/RTWCore89.cpp',core)
print('Passive diagnostics added, protocol accept/reject/encryption behavior preserved.')
