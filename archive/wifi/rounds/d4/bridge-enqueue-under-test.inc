static void queueReceiveFrame(void *opaque, struct sk_buff *skb)
{
    auto *context = static_cast<Rtw89LinuxContext *>(opaque);
    if (skb && context && context->queueReceive &&
        context->queueReceive(context->nativeOwner, skb, skb->len))
        return; // The native queue now owns the entire, unchanged skb.
    kfree_skb(skb);
}

