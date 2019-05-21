### Sisyphus

This is a simple [Raft consensus algorithm](https://raft.github.io/) implementation and used for KV store purpose.   

The development task has not completed yet, this implementation currently supports RequestVote, AppendEntries, InstallSnapshots RPCs, it does not support dynamic membership configure update, but it can basically run now.   

It is developed based on tzrpc, which support async client invoking now, and this feature can greately simplify the whole system architecture design. The consensus algorithm core code is almost the same as its author’s original implementation, so we can assume our implementation is right arbitrarily. Perhaps I will try to PASS the [jepsen test](https://github.com/jepsen-io/jepsen) in the future.  
LevelDB is used for Raft log entries and meta data duration, and is also used for StateMachine storage, and this means sisyphus aims for stable KV storage usage. We abstracted client request as RPC interface, and it can atomically forward the request to the desired peer when the node connected is not Leader.   


*** Performance Benchmark ***
Aliyun ecs.xn4.small (1C1G1M)   
Update TPS: 257   
Select TPS: 1261   
Update Select mix TPS: 120 + 690