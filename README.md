### Sisyphus

This is a simple Raft [consensus algorithm](https://raft.github.io/) implementation.   

The development task has not completed yet, this implementation supports RequestVote, AppendEntries, InstallSnapshots RPC, it doesnot support dynamic membership configure, but it can basically run now.   

It is developed based on tzrpc, which support async client invoking now, and this feature can greately simplify whole system architecture design. The consensus algorithm code is almost the same as its author’s original implementation, so we can assume it is right arbitrarily.   
LevelDB is used for Raft log entries and meta data duration, and is also use for StateMachine storage, and this means sisyphus aims for stable KV storage usage. We abstracted client request as RPC interface, and it can atomically forward the request to the desired peer when the node connected is not Leader.   
