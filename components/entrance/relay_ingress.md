# Owned relay ingress

This implementation is used only by `ENTRANCE_LIGHTS=1`. The default-off build
retains the original four fixed messages, consumer buffer and fatal queue-full
behavior. The frame assembler and 2048-byte wire limit are unchanged.

Five ownership slots cover four queued messages and one consumer. A producer
must reserve a slot before copying or allocating, so a full queue cannot cause
a transient sixth payload allocation. Every slot has a 513-byte slab for a
payload of at most 512 bytes plus its terminator. Larger messages allocate
exactly their length plus one. The FreeRTOS queue copies an eight-byte C3
descriptor; it never borrows the frame assembler's reusable buffer.

Only the ownership bit mask is accessed inside the short critical sections.
Allocation, copying, queue operations, parsing, signing and socket sends stay
outside those sections. The WebSocket callback neither parses JSON nor waits
for a peak reservation. A large packet takes the existing zero-wait shared peak
reservation and checks internal 8-bit heap before allocating. The guard retains
32 KiB free and a 24 KiB largest block, with the allocation's bytes and another
1 KiB added to both requirements. These remain conservative admission rules,
not a measured guarantee about all concurrent runtime allocations.

The reservation ends immediately after allocation and copying. It is not held
while a message waits in the queue. Subsequent SRP/TLS admission therefore sees
the already-retained payload in its current heap measurement. Slab messages do
not allocate or take that reservation: canonical game commands, status,
challenge, disconnect and printable unverified guest names fit this path.
Signed-profile joins and larger lighting messages use the guarded path.

Resource refusal discards the complete, unaccepted message and increments the
authenticated relay status counters `ingress_refused`, `ingress_peak_refused`,
`ingress_memory_refused` or `ingress_capacity_refused`. It does not set the
shared socket's fatal flag. Existing caller deadlines report failure; a timed
out lighting request ends that lighting connection and discards its Security2
state. No ciphertext is replayed. Malformed framing or a malformed message
accepted for parsing still follows the existing fatal protocol-error behavior.

The consumer releases its descriptor after handling, including parse failure.
An enqueue failure releases the new owner immediately. Disconnect cleanup
first stops/destroys the SDK event producer, then drains and releases the four
queue entries, then resets the frame assembler. Resetting a queue without
releasing its descriptors would leak memory/owners. Pool initialization is only
valid before a producer starts with no outstanding descriptors. A descriptor
has one owner and must not be duplicated and released twice; release clears the
consumed descriptor in place and is harmless when called again on that cleared
value.

## Memory tradeoff

The previous C3 queue-item storage was `4 * 2056 = 8224` bytes, with another
2056-byte consumer allocation, totaling 10280 bytes. Queue-item storage is now
`4 * 8 = 32` bytes; the consumer descriptor uses its existing task stack. The
pool is `5 * 513 + 1 = 2566` bytes. This removes 10248 bytes of retained dynamic
payload storage and adds 2566 bytes of fixed pool storage: a **7682-byte gross
idle reduction**, before alignment, mutex/diagnostic fields, allocator metadata
and unchanged queue control storage. The linked C3 build must establish the
actual static/heap delta; no physical heap result is implied by this arithmetic.

Five maximum-size large messages can retain `5 * 2049 = 10245` dynamic bytes,
including a processing message. Together with the unused slabs and queue
descriptors this is 12843 bytes, **2563 bytes more** than the previous fixed
payload storage, before allocator overhead and integration fields. This is a
bounded burst tradeoff for the lower idle footprint. Heap and peak admission can
refuse a new large message; there is no promise to accept all four large queued
messages under resource pressure, and no threshold has been lowered to force
acceptance.

## Checks

`python3 -m unittest discover -s tests -p 'test_relay.py' -v` compiles the exact
ownership module with ASan/UBSan. It exercises size/heap boundaries, mixed FIFO,
four queued plus one processing, refusal before a sixth allocation, queue-full
cleanup, peak-busy small controls, heap/allocator failures, a five-packet maximum
large burst, real-parser malformed messages, 1000 reconnect/drain cycles and
20000 mixed-size producer attempts with a concurrent consumer. The existing
fragmentation and transport failover checks run alongside it.

These host checks cover ownership and bounds, not FreeRTOS scheduling, real
allocator fragmentation, radio coexistence or post-activation SRP headroom.
Candidate target compilation and physical heap/latency checks remain separate.
