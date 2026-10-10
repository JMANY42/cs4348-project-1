# CS 4348 PROJECT 1 MILESTONE 1 SUBMISSION
Valerie Nelson and David Lewis

## What we submitted

We submitted four skeleton programs (client.c, dispatcher.c, logger.c, and worker.c) that do not have functinoality yet but connect to one another via the interprocess communication methods. These methods are coordinated through the seperate processes via a common header file and ipc.c. 

ipc.c defines: 
* client -> dispather named pipe
* dispather -> worker message queue
* worker -> dispatcher, logger shared memory and signal


To be done for milestone 3:
* Functinality of jobs requested by client
* Inter process error handleing 


## To Run
Run 
```
make
```
to compile all four binaries. 

Then, in seperate terminals, run 
```
./client
./dispatcher
./logger
./worker
```
to see them in action.