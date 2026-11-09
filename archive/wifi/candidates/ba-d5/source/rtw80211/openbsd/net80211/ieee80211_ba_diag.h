/* Local passive BA diagnostics. Contains no addresses, payloads or keys.
 * All accesses on the d4 native protocol gate. Never bypass protocol checks. */
#ifndef WTL_IEEE80211_BA_DIAGNOSTICS_H
#define WTL_IEEE80211_BA_DIAGNOSTICS_H
enum ieee80211_wtl_ba_diag {
    WTL_BA_TxRequest,
    WTL_BA_TxRequestQueued,
    WTL_BA_TxRequestQueueError,
    WTL_BA_TxResponseTimeout,
    WTL_BA_TxResponse,
    WTL_BA_TxResponseShort,
    WTL_BA_TxResponseNoRequest,
    WTL_BA_TxResponseTokenMismatch,
    WTL_BA_TxResponseRefused,
    WTL_BA_TxHardwareFailure,
    WTL_BA_TxAgreement,
    WTL_BA_RxRequest,
    WTL_BA_RxNotReady,
    WTL_BA_RxNonHT,
    WTL_BA_RxShort,
    WTL_BA_RxBusy,
    WTL_BA_RxRefused,
    WTL_BA_RxHardwareFailure,
    WTL_BA_RxAgreement,
    WTL_BA_RxActionValidated,
    WTL_BA_RxAPAction,
    WTL_BA_RxAPProtectedAction,
    WTL_BA_RxUnprotectedMgmtDrop,
    WTL_BA_RxMgmtDecryptFailure,
    WTL_BA_RxProtectionOffDrop,
    WTL_BA_LastTxParams,
    WTL_BA_LastRxParams,
    WTL_BA_LastRxResponseStatus,
    WTL_BA_COUNT
};
#define WTL_BA_INC(ic, field) (++(ic)->ic_wtl_ba_diag[WTL_BA_##field])
#endif
