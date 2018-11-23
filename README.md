# Dynamic Training with MXNet

Dynamic Training (DT) is an experimental fork of MXNet that you can use for dynamically scaling your training jobs.

To use the dynamic training feature you need an instance to serve as your master.
You can then add workers at any time to increase your cluster size, even while training.

You can setup Dynamic Training with a Cloud Formation Template (CFT) or with the AWS CLI tool.
Tutorials for each route are provided.

* In the [CFT tutorial](#How-to-Setup-Dynamic-Training-with-a-Cloud-Formation-Template), you launch an initial cluster of two instances - a master and a worker.
Then you add new workers through the EC2 console by using the Launch from Template option.

* In the [Manual setup tutorial](#How-to-Setup-Dynamic-Training-Manually), you launch a master, configure it, and then add workers.


## Contents of this Guide

* [Plan your cluster](#plan-your-cluster)
* [Setup DT with CFT tutorial](#How-to-Setup-Dynamic-Training-with-a-Cloud-Formation-Template)
* [Setup DT manually tutorial](#How-to-Setup-Dynamic-Training-Manually)
* [Logs](#logs)
* [Distributed training scripts](#distributed-training-scripts)
* [Troubleshooting](#troubleshooting)


## Plan Your Cluster

So you want use Dynamic Training, but you need a plan!
There are four areas to plan regardless if you use CFT or the AWS CLI: accessing the nodes, setting up a shared file system, getting the DT package, and what data prep script to use.

For example, if you have two nodes - master & worker:
1) You can ssh into the master node, then from master, you can ssh into the worker.
2) You have a shared file system, which master & worker can access.
3) You have installed MXNet Dynamic Training pip package OR built it from source on the master node.
4) You have a data preparation script that new workers will use to get ahold of training data.

### Access
Dynamic Training's cluster setup assumes you have your master and any number of worker nodes.
The most basic setup is one master and one worker, making a two node cluster.
You administer everything from the master by SSH.
The workers don't have public IPs,
so if you need to access a worker, you do this from the master.

### Shared File System
Also, all of the nodes need access to the dataset through a shared file system.

### MXNet Dynamic Training Package
Use pip to install the package, or build the package from source.
* [Wheel file of mxnet_cu92mkl-1.3.1 with Dynamic Training](https://s3-us-west-2.amazonaws.com/us-west-2-aws-dl-cfn/mxnet_cu92mkl-1.3.1-py2.py3-none-manylinux1_x86_64.whl)

To install with pip (recommended):
```bash
pip install -e https://s3-us-west-2.amazonaws.com/us-west-2-aws-dl-cfn/mxnet_cu92mkl-1.3.1-py2.py3-none-manylinux1_x86_64.whl
```

To build from source:
```bash
git clone https://github.com/awslabs/dynamic-training-with-apache-mxnet-on-aws.git
cd dynamic-training-with-apache-mxnet-on-aws
make
```

**Note:** For specific settings and configurations for your platform, refer to [MXNet documentation](https://mxnet.incubator.apache.org/install/build_from_source.html).

### Data Preparation

When new workers are added to cluster that is actively training,
they need to be provided with the training data.

For this purpose you need a data preparation script which is deployed with each new worker.

You can write any "prepare data" code which will be run before worker is added to cluster.
The worker will be added to cluster only if this script is successfully executed.

* [Example data preparation script](prepare-data.py) - this script downloads the CIFAR10 dataset.


## How to Setup Dynamic Training with a Cloud Formation Template

### Part 1: Setup a distributed training cluster
This part will setup a master and one worker.

**Step 1:** Go to [AWS Cloud Formation](https://console.aws.amazon.com/cloudformation/home)
**Step 2:** Click the *Create Stack* button.
**Step 3:** At next page, choose the option "Upload a template to Amazon S3"
**Step 4:** Upload [dynamic-training-cft.json](https://raw.githubusercontent.com/awslabs/dynamic-training-with-apache-mxnet-on-aws/dynamic-training-cft.json) from your desktop. Click Next.
**Step 5:** At next page, fill in the template, clicking through the options and agreement page until you get to the *Create* button.

The following are guidelines for this page:

**StackName**- your choice
**EFSFileSystemId**: leave it blank
**ImageType**: Ubuntu
**InstanceType**: your choice
**KeyName**: use the dropdown to select the ssh key you want to use
**PrepareDataScriptPath**: leave the default value
**SSHLocation**: your public IP or range of IPs that may access the master node over SSH (see tip)

**Tip**: To lookup your SSH location go to [EC2 security group page](https://console.aws.amazon.com/ec2/v2/home#SecurityGroups:sort=groupId).
Click *Create security group*.
Click *Add Rule*.
Select *My IP* in Source tab. Copy that IP, with 24 bit mask.
For example, if your IP is 192.168.1.200, use 192.168.1.0/24 to allow anyone within that range access the master.

Creation my take five minutes or more.

**Step 6:** After the creation of the cluster is finished,
you need to find the public IP of the master node.
To get this info, from the Cloud Formation page, click the Resources tab, and look for MasterAutoScalingGroup.
Click on it, then click the Instances tab, and then click on instanceID.

**Step 7:** Setup ssh-key forwarding:

```bash
ssh-add -K ~/your_key_file.pem
```

**Step 8:** Login to the master node:

```bash
ssh -o ServerAliveInterval=60 -A ubuntu@IP_ADDRESS_OF_MASTER
```

**Step 9:** If needed, update the data preparation script that was referenced in the PrepareDataScriptPath from Step 5. Otherwise download [prepare-data.py](prepare-data.py) to `/myEFSvolume`.
The provided example will download and prepare the CIFAR10 dataset.
You can use curl to grab this example and place it in the default location.

```bash
curl -o /myEFSvolume/prepare-data.py https://raw.githubusercontent.com/awslabs/dynamic-training-with-apache-mxnet-on-aws/prepare-data.py
```

**Step 10:** Check the environment variable `LD_LIBRARY_PATH` and make sure it has the `cuda-9.2` path.
If not, add it with the following:

```bash
export LD_LIBRARY_PATH=/usr/local/cuda-9.2/lib64:$LD_LIBRARY_PATH
```

**Step 11:** Copy a distributed training script to the shared volume:

```bash
cp /home/ubuntu/.local/lib/python3.6/site-packages/mxnet/tools/launch.py /myEFSvolume
```

**Step 12:** Check the GPU count.

```bash
echo $DEEPLEARNING_WORKER_GPU_COUNT
```

**Step 13:** If there are no GPUs the output of the previous step is `0`.
If it is `0`, then delete the `--gpus` parameter in the following script.
Otherwise, run the script as is:

```bash
TRAINING_CMD="python /home/ubuntu/.local/lib/python3.6/site-packages/image-classification-example/train_cifar10.py --gpus $(seq -s , 0 1 $(($DEEPLEARNING_WORKER_GPU_COUNT - 1))) --network resnet --num-layers 50 --kv-store dist_device_sync"
/myEFSvolume/launch.py -n $DEEPLEARNING_WORKERS_COUNT -H $DEEPLEARNING_WORKERS_PATH --elastic-training-enabled True python /home/ubuntu/.local/lib/python3.6/site-packages/image-classification-example/train_cifar10.py --gpus $(seq -s , 0 1 $(($DEEPLEARNING_WORKER_GPU_COUNT - 1))) --network resnet --num-layers 50 --kv-store dist_device_sync
```

The output will look something like this:

```bash
2018-11-16 22:46:47,496 INFO Created worker host file /myEFSvolume/host_worker
2018-11-16 22:46:47,518 INFO Created worker host log file /myEFSvolume/host_worker_log
2018-11-16 22:46:47,525 INFO Launching server node at ip: 10.0.0.25
2018-11-16 22:46:47,533 INFO Launching server node at ip: 10.0.1.207
2018-11-16 22:46:47,533 INFO Launching worker node at ip: 10.0.0.25
2018-11-16 22:46:47,536 INFO Launching worker node at ip: 10.0.1.207
```


### Part 2: Add More Workers to a Training Cluster

**Step 1:** Go to the [EC2 Console](https://console.aws.amazon.com/ec2/v2/home#LaunchTemplates:sort=launchTemplateId), then open the *Launch Template* tab.

**Step 2:** Find the Launch Template with the name you provided when you created the Stack.
**Step 3:** Launch more instances with this template. Select the template version at next page. Everything else will be pre-filled after you select the template version. Then click *Launch instance from template*.

While instances are launched and added to the training cluster,
you can watch the `elastic_worker_status` tag on the newly launched instances.
This will let you know once the instance is added to the cluster.
`ADDED` means the instances were successfully added to cluster.

In logs you will see lines like:

```bash
Launching new worker jobs :ssh -A -o StrictHostKeyChecking=no 172.31.65.62 -p 22
```

In the workers' host log file there will be entries like:

```bash
0 ADDED 172.31.65.62 1542536656974259605
```

### Part 3: Removing Workers from a Training Cluster

To find all the instances in the cluster that can be removed,
open the EC2 console and select the instance page.
Search for instances with tag key. Replace <STACK-NAME> with your own stack name:

```bash
tag:<STACK-NAME>-aws-dl-cfn-elastic-worker : All values
```

To remove any instance, select that instance from this view and go to the *Tags* tab.
Click *Add/Edit* tags, then remove the tag `<STACK-NAME>-aws-dl-cfn-elastic-worker`.
Click *Save*.

The instance will be removed, and once this is complete the `elastic_worker_status` tag will have value `REMOVED`.
The instance will be terminated automatically after getting removed.


## How to Setup Dynamic Training Manually

When you start this process, you launch a single instance.
This will be your master node.
Make sure you login into your master node using the `-A` flag.

```bash
ssh -A ubuntu@IP_ADDRESS_OF_MASTER
```

### Part 1: Setup the Cluster

**Step 1:** Use the following bash function to run commands on every node in the cluster.
Running this for now and then you can use `runclust` in later steps.

```bash
function runclust(){ while read -u 10 host; do host=${host%% slots*}; ssh -o "StrictHostKeyChecking no" $host ""$2""; done 10<$1; };
```

**Step 2:** Create a hosts file that has the private IPs of every node in the cluster.
Place the private IP of the master node last in the list.
That way you can keep track of it. When removing nodes, you never want to remove the master accidentally.
Save it in your master node's home directory.
It will look something like this:

```bash
172.100.1.200
172.200.8.99
172.48.3.124
```

**Step 3:** Run pip install across the cluster.

```bash
runclust "pip install --user --force-reinstall -e https://s3.amazonaws.com/us-east-1-aws-dl-cfn/mxnet_cu92mkl-1.3.1-py2.py3-none-manylinux1_x86_64.whl"
```

**Step 4:** Kill any Python processes across the cluster.

```bash
runclust "sudo pkill python"
```

**Step 5:** Create a shared file system called `myEFSvolume`.

[Follow this EFS walkthrough](https://docs.aws.amazon.com/efs/latest/ug/wt1-getting-started.html).

**Step 6:** Copy your hosts file to the root of your new EFS volume.

```bash
cp ~/hosts /myEFSvolume
```

**Step 7:** Copy `launch.py` to `myEFSvolume`.
This step assumes you're on Ubuntu. Modify the path according to your system.

```bash
cp /home/ubuntu/.local/lib/python3.6/site-packages/mxnet/tools/launch.py /myEFSvolume
```

**Step 8:** Download [prepare-data.py](prepare-data.py) to `/myEFSvolume`.
The provided example will download and prepare the CIFAR10 dataset.
You can use curl to grab this example and place it in the default location.

```bash
curl -o /myEFSvolume/prepare-data.py https://raw.githubusercontent.com/awslabs/dynamic-training-with-apache-mxnet-on-aws/prepare-data.py
```

**Step 9:** Check the GPU count.

```bash
echo $DEEPLEARNING_WORKER_GPU_COUNT
```

**Step 10:** If there are no GPUs the output of the previous step is `0`.
If it is `0`, then delete the `--gpus` parameter in the following script.
Otherwise, run the script as is:

```bash
TRAINING_CMD="python /home/ubuntu/.local/lib/python3.6/site-packages/image-classification-example/train_cifar10.py --gpus $(seq -s , 0 1 $(($DEEPLEARNING_WORKER_GPU_COUNT - 1))) --network resnet --num-layers 50 --kv-store dist_device_sync"
/myEFSvolume/launch.py -n $DEEPLEARNING_WORKERS_COUNT -H $DEEPLEARNING_WORKERS_PATH --elastic-training-enabled True python /home/ubuntu/.local/lib/python3.6/site-packages/image-classification-example/train_cifar10.py --gpus $(seq -s , 0 1 $(($DEEPLEARNING_WORKER_GPU_COUNT - 1))) --network resnet --num-layers 50 --kv-store dist_device_sync
```

The output will look something like this:

```bash
2018-11-16 22:46:47,496 INFO Created worker host file /myEFSvolume/host_worker
2018-11-16 22:46:47,518 INFO Created worker host log file /myEFSvolume/host_worker_log
2018-11-16 22:46:47,525 INFO Launching server node at ip: 10.0.0.25
2018-11-16 22:46:47,533 INFO Launching server node at ip: 10.0.1.207
2018-11-16 22:46:47,533 INFO Launching worker node at ip: 10.0.0.25
2018-11-16 22:46:47,536 INFO Launching worker node at ip: 10.0.1.207
```


### Part 2: Add New Workers to the Cluster

**Step 1:** Launch a new instance from the EC2 console.
**Step 2:** Add the new instance's private IP address to the hosts file.
**Step 3:** Prepare data on new instance.
From the master node, ssh into the new instance and run the data prep script.

```bash
curl -o /myEFSvolume/prepare-data.py https://raw.githubusercontent.com/awslabs/dynamic-training-with-apache-mxnet-on-aws/prepare-data.py
python prepare_data.py
```

**Step 4:** Copy the updated hosts file over the `/myEFSvolume/host_worker` file.

```bash
cp hosts /myEFSvolume/host_worker
```

The `host_worker` file should now look like:

```bash
new-node-ip-address
node-ip-address
node-ip-address
master-node-ip-address
```

**Step 5:** In next epoch the worker will be added, and a log line entry will be added to `/myEFSvolume/host_worker_log`.
The entry in `host_worker_log` will look like:

```bash
0 IP ADDED <epoch>
```

### Remove a Worker from the Cluster

Find the worker that needs to be removed in the `/myEFSvolume/host_worker` file and delete it.
Be careful about what you are removing and make sure that you don't remove the master node's IP.
Training will halt if you remove the master node.

In next epoch, worker will be removed and a log line entry will be added to `/myEFSvolume/host_worker_log` file
The log entry will have entry like this:

```bash
1 REMOVED_IP REMOVED <epoch>
```


## Logs

These files are created in the directory where `launch.py` is used.
In the examples here this is in the shared volume, `myEFSvolume`.

* `/myEFSvolume/host_worker` is created when training is launched. This contains entries for the current workers
* `/myEFSvolume/host_worker_log` contains entries about workers if they were added or removed


## Distributed Training Scripts

You can write and use your own distributed training scripts with DT.

### Placement
Place your training script in the shared volume, so that every node in the cluster can access the training code.
As you will see in the examples, you will have a copy of `launch.py` in the share volume as well.

### Writing a Distributed Training Script
To write a distributed training script you need to create a `kvstore` of type `dist_device_sync` or `dist_sync`.

```
// note that for distributed training case we pass --kv-store dist_device_sync
// to our training script, dist_device_sync will tell the code that it is distributed
// training
kv = mx.kvstore.create(args.kv_store)
```

You need to implement the `mx.mod.BaseDataIterator` interface and implement a `get_data_iterator` function.
See example below, how we created `ETDataIterator`.
Pass this `ETDataIterator` when creating a `Module` object.

```python
class ETDataIterator(mx.mod.BaseDataIterator):
    def __init__(self, args, data_loader):
        self._args = args
        self._data_loader = data_loader
    def get_data_iterator(self, kv):
        # data iterators
        (train, val) = self._data_loader(self._args, kv)
        if 'dist' in self._args.kv_store and not 'async' in self._args.kv_store:
            epoch_size = get_epoch_size(self._args, kv)
            logging.info('Resizing training data to %d batches per machine', epoch_size)
            # resize train iter to ensure each machine has same number of batches per epoch
            # if not, dist_sync can hang at the end with one machine waiting for other machines
            train = mx.io.ResizeIter(train, epoch_size)
        return train,val

data_iterator = ETDataIterator(args, data_loader/*data loader is function which customer writes even today*/)
# create model
model = mx.mod.Module(
context=devs,
symbol=network
symbol=network,
data_iterator=data_iterator
)
```

After this, you can call the `fit` function of `Module`.

```python
# run
model.fit(train,
begin_epoch=args.load_epoch if args.load_epoch else 0,
num_epoch=args.num_epochs,
eval_data=val,
eval_metric=eval_metrics,
kvstore=kv,
optimizer=args.optimizer,
optimizer_params=optimizer_params,
initializer=initializer,
arg_params=arg_params,
aux_params=aux_params,
batch_end_callback=batch_end_callbacks,
epoch_end_callback=checkpoint,
allow_missing=True,
monitor=monitor)
```

### Example Scripts

TODO: Add links to scripts


## Troubleshooting

### Common errors when creating a cloud formation stack
1) VPC limit exceeded - you may be hitting default limit of 5 VPC's in your account
2) Stuck in Master/Worker Autoscaling group - a probable reason is your instances are failing to launch. You can check that by going to the EC2 console AutoScalingGroup settings.
Search for the name of your stack. You will see entries like `<stack-name>-WorkerLaunchConfig-.....` and  `<stack-name>-MasterLaunchConfig-...`
You can select them, and view Instances tab and Activity History tab to see what is wrong.
3) Cloud Formation setup is complete, but you are not able to ssh to the master or other nodes. There can be multiple reasons for this:
    3.1) Go to EC2→Security Groups page. Search for <stack-name>,
    and you should be able to see a security group with the name `<stack-name>_SSH`>.
    Select this stack. Go to Inbound, Edit, Add Rule (Type: SSH, Source:MyIp).
    It is recommended that you allow a subnet. For example for 172.12.33.24,
    the setting would be 172.12.33.0/24.
    3.2) You may be using wrong ec2-key-pair to ssh. Go to Ec2→Instances.
    Search for `<stack-name>`, and you should see an instance named `<stack-name>-Master`.
    Select that instance, and under Description tab, you will see key-pair-name.
    Make sure that you are using same key pair to login to the EC2 instance.
    3.3) You can SSH to the master node, but not to a worker node.
   Make sure that while ssh-ing to the master node, you used the `-A` option to forward your ssh key-pair.
   Make sure you are using private IP of worker instances for ssh. You can find that in /opt/deeplearning/workers .
   You can ssh to workers only through the master node.
    3.4) You can login to the master node and worker node, but something else is wrong.
  Verify that the Cloud Formation setup was completed successfully. For each instance, check the logs:
  `/var/log/cloud-init-output.log`, `/var/log/cfn-init-cmd.log`, and `/var/log/cfn-init.log`.
  Look for any errors in the logs.
4) Check environment variables with `env`.
It should have these variable set: ELASTIC_WORKER_TAG, WORKER_LAUNCH_TEMPLATE_ID, DEEPLEARNING_WORKERS_PATH=/opt/deeplearning/workers, DEEPLEARNING_WORKERS_COUNT=2, AWS_REGION, DEEPLEARNING_WORKER_GPU_COUNT, EFS_MOUNT
5) Make sure that you have the launch.py script and your training code on shared the shared EFS volume, i.e, `/myEFSvolume.`
6) A new worker is not getting added. The tag expected isn't appearing: `elastic_worker_status : ADDED`
Make sure that you launched the worker. Make sure that new worker instance is running. Note the private IP of instance and verify it in the hosts file. Check `<dir-of-launch.py>/host_worker` file and verify that the private IP of the instance is added to this file. Check the logs.

If you see the private IP, check the `<dir-of-launch.py>/host_worker_log` file, and see if it has an entry regarding private IP of newly launched instance. If no, then it is fine. Move on to next steps. If an entry says ADDED, this instance is already added.

If there is no entry in `<dir-of-launch.py>/host_worker_log` file, the instance may be preparing data.
Login to the new node (from the master node, use `ssh private-ip`).
Verify the steps for a newly launched instance.
Check that the prepared data is either successful, or if that Python process is still running. In case it was successful, there should be a 0 byte file created in `/myEFSvolume/prepare_data_success`.
If you don't find this success file and you have made sure that data has been prepared, and there are no other errors in the logs, you can create this file manually. Any new node should be picked up in the next epoch.

**Report any issues here in the GitHub repo.**
