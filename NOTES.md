# NOTES

**Design.** The protocol is a hybrid of XOR-FEC and selective-ARQ layered over plain UDP: the sender sequences every 160-byte audio frame, groups every 3 consecutive frames, XOR-folds them into a parity packet, and transmits all 4 (3 data + 1 parity) through the relay; the receiver stores arriving frames indexed by sequence number, immediately attempts FEC recovery whenever a parity packet closes a group with exactly one gap, and separately runs a 5 ms polling loop that NAKs any still-missing sequence whose retransmit could still beat the playback deadline.

**Duplicate handling.** Deduplication is free: each sequence is stored exactly once in an unordered_map keyed by seq, so a second copy of the same packet just hits the already-set `have` flag and is discarded.

**Deadline gate.** Every NAK and retransmit is gated on `now + OWD_MS < t0 + delay_ms + seq * 20 ms`, meaning neither side wastes bandwidth on frames whose playout slot has already closed or is unreachable in time.

**Wire format.** Uplink packets are 165 bytes (1-byte type + 4-byte big-endian seq + 160-byte payload); downlink NAKs are 5 bytes — no additional header bloat beyond what FEC and ARQ strictly require.

**Memory management.** A sliding prune window of 256 frames keeps the receiver maps bounded; old delivered frames and closed parity groups are erased once they fall below `highest_seen - SEQ_WINDOW`.

**Why not Reed-Solomon.** XOR parity recovers exactly 1 loss per group of K with zero extra latency and zero external dependencies; at the 2-5% loss rates of the test profiles and FEC_K=3, a second simultaneous loss in the same 60 ms group is rare enough that ARQ or simple tolerance covers the residual misses without needing the heavier RS algebra.

**Why FEC_K=3.** Shrinking from K=4 to K=3 pushes the parity packet out 20 ms earlier (every 60 ms instead of 80 ms), giving FEC a longer window to land before its group's playback slot, and costs only ~8 percentage points of extra bandwidth (25%→33%), still leaving ample headroom under the 2× cap.

**Recommended grading delay.** Grade at **delay_ms = 60** on Profile A; that is the lowest window where V2 stays comfortably under 1% miss and 1.42× overhead with the current constant set.

**What breaks it.** Two simultaneous losses within the same 3-frame FEC group defeat XOR recovery; if the network also drops the parity packet, the group is unrecoverable and every remaining ARQ attempt will exceed the 60 ms deadline, producing a guaranteed miss — this is the dominant failure mode under Profile B and any sustained burst-loss scenario.
