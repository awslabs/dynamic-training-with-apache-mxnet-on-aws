#!/usr/bin/python
import time
import os
import multiprocessing
def mxnet_worker():
    print("pid: {}".format(os.getpid()))
    time.sleep(10)
    b_time = time.time()
    import mxnet 
    print("time consumes: {}".format(time.time()-b_time))
 
read_process = [multiprocessing.Process(target=mxnet_worker) for i in range(8)]
for p in read_process:
    p.daemon = True
    p.start()
   # time.sleep(5)
time.sleep(100)
