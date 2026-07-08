Implemented Features
The following features have been implemented:
Disk Scheduling
•	Implemented a disk request queue (disk_queue) to buffer pending disk operations. 
•	Added support for two disk scheduling policies: 
o	FCFS (First Come First Serve): Requests are served in arrival order. 
o	SSTF (Shortest Seek Time First): The request closest to the current disk head is selected. 
•	Added set_disk_policy() system call to switch between FCFS and SSTF dynamically. 
•	Maintains current_head to track disk head position and compute seek distance. 
Swap System Enhancements
•	Added mechanism to generate disk read/write requests from swap operations. 
•	Triggers disk activity under memory pressure via page swapping. 
RAID Support
•	Implemented basic RAID levels: 
o	RAID 0 (Striping): Data distributed across disks using modulo mapping. 
o	RAID 1 (Mirroring): Data duplicated across disks for redundancy. 
o	RAID 5 (Parity-based redundancy): 
	Computes parity using XOR. 
	Supports reconstruction of data if a disk fails. 
Statistics Tracking
•	Tracks: 
o	Number of disk reads and writes 
o	Total latency of requests 
o	Total number of disk operations 
________________________________________
Design Decisions and Assumptions
Disk Queue Design
•	A fixed-size array (disk_queue[MAX_REQ]) is used to store pending requests. 
•	qsize tracks the number of active requests. 
•	Requests are removed by swapping with the last element for O(1) deletion. 
Scheduling Design
•	Priority-based scheduling is applied first using proc->level (MLFQ integration). 
•	Within the same priority: 
o	FCFS preserves insertion order. 
o	SSTF selects the request closest to current_head. 
Assumptions
•	Disk is modeled as a linear array of blocks. 
•	Head movement cost is proportional to absolute distance: 
o	|current_head - logical_block| 
•	All processes have a valid level field (MLFQ integration assumed). 
•	Maximum number of pending requests is bounded by MAX_REQ. 
RAID Design
•	RAID 0 assumes perfect disk availability (no redundancy). 
•	RAID 1 assumes identical mirror writes. 
•	RAID 5 assumes single-disk failure tolerance using XOR parity. 
Simplifications
•	No real hardware parallelism is simulated. 
•	RAID reconstruction assumes synchronous access. 
•	No persistent crash recovery mechanism is implemented. 
________________________________________
3. Experimental Results
FCFS vs SSTF Behavior
FCFS Output Characteristics:
•	Requests are executed strictly in arrival order. 
•	Simple but may cause high disk head movement. 
•	Higher average seek time under random workloads. 
SSTF Output Characteristics:
•	Always selects nearest block to current head. 
•	Significantly reduces average seek distance. 
•	Can cause starvation for distant requests in worst cases. 
________________________________________
Observed Behavior
•	Under sequential workloads: 
o	FCFS and SSTF perform similarly. 
•	Under random workloads: 
o	SSTF shows noticeably lower total latency. 
•	Disk head movement is reduced under SSTF due to locality exploitation. 
________________________________________
RAID Results
RAID 0
•	Fastest performance 
•	No redundancy (data loss if disk fails) 
RAID 1
•	Write overhead increased due to duplication 
•	Read can be parallelized conceptually 
RAID 5
•	Balanced storage efficiency and redundancy 
•	Correctly reconstructs missing data using XOR parity 
•	Slight performance overhead due to parity computation 
________________________________________
Observations
•	SSTF improves average disk seek performance compared to FCFS. 
•	Priority-based scheduling ensures higher-level processes are served first. 
•	RAID 5 reconstruction successfully restores data in simulated failure scenarios. 
•	Swap-triggered disk activity correctly generates scheduling workload. 
________________________________________
Conclusion
The system successfully integrates:
•	Disk scheduling policies (FCFS, SSTF) 
•	Swap-driven disk requests 
•	Multi-level RAID functionality 
This demonstrates how OS-level scheduling decisions significantly affect disk performance and system throughput.

