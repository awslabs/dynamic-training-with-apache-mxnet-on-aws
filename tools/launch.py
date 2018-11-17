#!/usr/bin/env python

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

"""
Launch a distributed job
"""
import argparse
import os, sys
import signal
import logging
import time
from threading import Thread

try:
    import boto3
except ImportError:
    import pip
    pip.main(['install', 'boto3'])
    import boto3

curr_path = os.path.abspath(os.path.dirname(__file__))
sys.path.append(os.path.join(curr_path, "../3rdparty/dmlc-core/tracker"))

def dmlc_opts(opts):
    """convert from mxnet's opts to dmlc's opts
    """
    args = ['--num-workers', str(opts.num_workers),
            '--num-servers', str(opts.num_servers),
            '--cluster', opts.launcher,
            '--host-file', opts.hostfile,
            '--sync-dst-dir', opts.sync_dst_dir]
    if opts.launch_worker is True:
        args.append('--launch-worker')
        args.append(str(opts.launch_worker))
    if opts.elastic_training_enabled is True:
        args.append('--elastic-training-enabled')
        args.append(str(opts.elastic_training_enabled))

    # convert to dictionary
    dopts = vars(opts)
    for key in ['env_server', 'env_worker', 'env']:
        for v in dopts[key]:
            args.append('--' + key.replace("_","-"))
            args.append(v)
    if dopts['elastic_training_enabled'] is True:
        args.append('--mxnet-launch-script-path')
        args.append(os.path.abspath(__file__))
        args.append('--worker-host-file')
        dirname = os.path.dirname(os.path.abspath(__file__))
        args.append(dirname + "/host_worker")

    if dopts['launch_worker'] is True:    
        #args.append('--launch-worker ' + True)
        args.append('--host')
        args.append(dopts['host'])
        args.append('--port')
        args.append(dopts['port'])
        
    
    args += opts.command
    try:
        from dmlc_tracker import opts
    except ImportError:
        print("Can't load dmlc_tracker package.  Perhaps you need to run")
        print("    git submodule update --init --recursive")
        raise
    dmlc_opts = opts.get_opts(args)

    return dmlc_opts


def manage_elastic_instance(worker_host_file, fixed_num_worker):

    remove_instances_queue = {}
    add_instances_queue = {}
    seqnum_seen_in_log = -1

    while(True):
        existing_elastic_workers = set()
        count = 0
        new_worker_set = []
        if not os.path.exists(worker_host_file):
            logging.info("Worker host file:{} doesn't exist yet. Sleeping for 5 sec".format(worker_host_file))
            time.sleep(5)
            continue
        with open(worker_host_file) as whf:
            for host_ip in whf:
                host_ip = host_ip.strip()
                if count < fixed_num_worker:
                    count = count + 1
                    new_worker_set.append(host_ip)
                    logging.debug("Adding host_ip:{} to new_worker_set".format(host_ip))
                    continue
                existing_elastic_workers.add(host_ip.strip())
                logging.debug("Adding hostIp:{} to existing worker set". format(host_ip))

        region = os.getenv('AWS_REGION')
        launch_template_id = os.getenv('WORKER_LAUNCH_TEMPLATE_ID')
        elastic_tag = os.getenv('ELASTIC_WORKER_TAG')

        efs_mount = os.getenv('EFS_MOUNT')
        elastic_worker_status_tag_key = 'elastic_worker_status'
        elastic_worker_status_removing_tag_value = 'REMOVAL_QUEUED'

        elastic_worker_status_adding_tag_value = "ADD_QUEUED"
        elastic_worker_status_added_tag_value = "ADDED"
        elastic_worker_status_removed_tag_value = "REMOVED"
        node_type = 'Worker'

        filters = [{'Name':'tag:aws:ec2launchtemplate:id', 'Values':[launch_template_id]},
            {'Name':'tag:NodeType', 'Values':[node_type]},
            {'Name': 'instance-state-name', 'Values': ['running']}]
        ec2 = boto3.client('ec2', region_name=region)
        response = ec2.describe_instances(Filters=filters)
        current_valid_elastic_worker_privateIps = {}
        removed_instances_tag_req = {}
        for reservation in response['Reservations']:
            for instance in reservation['Instances']:
                should_add_remove_tag = True
                tags = instance['Tags']
                privateIp = instance['PrivateIpAddress']
                has_elastic_tag = False
                has_elastic_status = False
                for kv in tags:
                    if kv['Key'] == elastic_tag:
                        has_elastic_tag = True
                    # get all the instances previously queued for removal
                    if kv['Key'] ==  elastic_worker_status_tag_key:
                        has_elastic_status = True
                    if kv['Key'] ==  elastic_worker_status_tag_key and elastic_worker_status_removing_tag_value == kv['Value']:
                        remove_instances_queue[privateIp] = instance['InstanceId']
                        logging.info("Got removing tag for host:{} indtanceId:{}".format(instance['PrivateIpAddress'], instance['InstanceId']))
                        should_add_remove_tag = False

                    elif kv['Key'] ==  elastic_worker_status_tag_key and elastic_worker_status_adding_tag_value == kv['Value']:
                        add_instances_queue[privateIp] = instance['InstanceId']
                        logging.info("Got adding tag for host:{} indtanceId:{}".format(instance['PrivateIpAddress'], instance['InstanceId']))

                if has_elastic_tag == True and has_elastic_status == False:
                    prepare_data_success_file_path = efs_mount + "/prepare_data_success_" + privateIp
                    # look for prepare data success file
                    if os.path.exists(prepare_data_success_file_path):
                        current_valid_elastic_worker_privateIps[privateIp] = instance['InstanceId']
                        logging.info("Found prepare data success file:{}. Adding privateip:{} to current_valid_elastic_worker".format(prepare_data_success_file_path, instance['PrivateIpAddress']))
                    else:
                        logging.info("Prepare data success file {} not found yet.".format(prepare_data_success_file_path)) 
                elif has_elastic_tag == True:
                    current_valid_elastic_worker_privateIps[privateIp] = instance['InstanceId']
                    logging.info("Adding privateip:{} to current_valid_elastic_worker".format(instance['PrivateIpAddress']))
                # this instance need to be queued for removal now
                if has_elastic_tag == False and should_add_remove_tag == True:
                    removed_instances_tag_req[instance['PrivateIpAddress']] = instance['InstanceId']
                    logging.info("Will add remove tag for host:{} indtanceId:{}".format(instance['PrivateIpAddress'], instance['InstanceId']))

        worker_host_log_file = worker_host_file + "_log"
        latest_removed_instance_ids = []
        if os.path.exists(worker_host_log_file):
            with open(worker_host_log_file) as whl:
                for line in whl:
                    # Each line should be in format SEQNUM ADDED|REMOVED HOSTIP TIME_SINCE_EPOCH
                    # delimiter is space ' '
                    splitted = line.split()
                    seqnum = int(splitted[0])
                    if seqnum <= seqnum_seen_in_log:
                        continue
                    if splitted[1] == 'ADDED':
                        if splitted[2] in add_instances_queue:
                            instanceId = add_instances_queue[splitted[2]]
                            ec2.create_tags(Resources=[instanceId], Tags=[{'Key':elastic_worker_status_tag_key, 'Value':elastic_worker_status_added_tag_value}])
                            add_instances_queue.pop(splitted[2])
                    if splitted[1] == 'REMOVED':
                        if splitted[2] in remove_instances_queue:
                            instanceId = remove_instances_queue[splitted[2]]
                            latest_removed_instance_ids.append(instanceId)
                            ec2.create_tags(Resources=[instanceId], Tags=[{'Key':elastic_worker_status_tag_key, 'Value':elastic_worker_status_removed_tag_value}])
                            remove_instances_queue.pop(splitted[2])
                    seqnum_seen_in_log = seqnum
        logging.debug("Last seqnum seen in worker host file log {}".format(seqnum_seen_in_log))

        if len(latest_removed_instance_ids) > 0:
            instaceIdsJoined = ",".join(latest_removed_instance_ids)
            logging.info("Terminating instance Ids: {}".format(instaceIdsJoined))
            ec2.terminate_instances(InstanceIds=latest_removed_instance_ids)

        hosts_to_remove = set()
        new_instance_add_queue = {}
        for host in existing_elastic_workers:
            if host in current_valid_elastic_worker_privateIps:
                new_worker_set.append(host)
                current_valid_elastic_worker_privateIps.pop(host)
                logging.debug("Added host:{} to new worker host".format(host))
            else:
                logging.info("Adding host:{} to hosts_to_remove".format(host))
                hosts_to_remove.add(host)

        for host in current_valid_elastic_worker_privateIps:
            new_worker_set.append(host)
            logging.info("Added host:{} to new worker host".format(host))
            new_instance_add_queue[host] = current_valid_elastic_worker_privateIps[host]

        #write this new_worker_set to worker_host_file
        with open(worker_host_file + ".tmp", 'w') as whf:
            for host in new_worker_set:
                host = host.strip()
                whf.write(host + "\n")
                logging.debug("Wrote host:{} to temp host file".format(host))
            logging.debug("Will rename temp host file to :{}".format(worker_host_file))
            os.rename(worker_host_file + ".tmp", worker_host_file)

        for host, instanceId in removed_instances_tag_req.items():
            if host in hosts_to_remove:
                ec2.create_tags(Resources=[instanceId], Tags=[{'Key':elastic_worker_status_tag_key, 'Value':elastic_worker_status_removing_tag_value}])
                logging.info("Created removing tags for host:{} instanceId:{}".format(host, instanceId))
        for host, instanceId in new_instance_add_queue.items():
            ec2.create_tags(Resources=[instanceId], Tags=[{'Key':elastic_worker_status_tag_key, 'Value':elastic_worker_status_adding_tag_value}])
            logging.info("Created adding tags for host:{} instanceId:{}".format(host, instanceId))

        logging.debug("Manage ET instance thread sleeping for 5 seconds")
        time.sleep(5)

def main():
    parser = argparse.ArgumentParser(description='Launch a distributed job')
    parser.add_argument('-n', '--num-workers', required=True, type=int,
                        help = 'number of worker nodes to be launched')
    parser.add_argument('-s', '--num-servers', type=int,
                        help = 'number of server nodes to be launched, \
                        in default it is equal to NUM_WORKERS')
    parser.add_argument('-H', '--hostfile', type=str,
                        help = 'the hostfile of slave machines which will run \
                        the job. Required for ssh and mpi launcher')
    parser.add_argument('--sync-dst-dir', type=str,
                        help = 'if specificed, it will sync the current \
                        directory into slave machines\'s SYNC_DST_DIR if ssh \
                        launcher is used')
    parser.add_argument('--launcher', type=str, default='ssh',
                        choices = ['local', 'ssh', 'mpi', 'sge', 'yarn'],
                        help = 'the launcher to use')
    parser.add_argument('--env-server', action='append', default=[],
                        help = 'Given a pair of environment_variable:value, sets this value of \
                        environment variable for the server processes. This overrides values of \
                        those environment variable on the machine where this script is run from. \
                        Example OMP_NUM_THREADS:3')
    parser.add_argument('--env-worker', action='append', default=[],
                        help = 'Given a pair of environment_variable:value, sets this value of \
                        environment variable for the worker processes. This overrides values of \
                        those environment variable on the machine where this script is run from. \
                        Example OMP_NUM_THREADS:3')
    parser.add_argument('--env', action='append', default=[],
                        help = 'given a environment variable, passes their \
                        values from current system to all workers and servers. \
                        Not necessary when launcher is local as in that case \
                        all environment variables which are set are copied.')
    parser.add_argument('--elastic-training-enabled', type=bool, default=False,
                        help = ' if this option is set to true, elastic training is enabled. \
                        If True, you should specify which instance pool to use by using option \
                        --instance-pool')
    parser.add_argument('--launch-worker', type=bool, default=False, help = 'whether this script should' \
                        'only launch worker instances')    
    parser.add_argument('--host', type=str, help='host name or ip of new worker host to launch')
    parser.add_argument('--port', type=str, default='22', help='port number of new worker for ssh command to run by')           
    parser.add_argument('command', nargs='+',
                        help = 'command for launching the program')

    args, unknown = parser.parse_known_args()

    args.command += unknown
    
    logging.info("BEGIN args %s", args)

    if args.num_servers is None:
        args.num_servers = args.num_workers

    args = dmlc_opts(args)
    
    logging.info("args after dmlc_opts %s", args)

    if os.getenv('WORKER_LAUNCH_TEMPLATE_ID') is not None and os.getenv('ELASTIC_WORKER_TAG') is not None and args.launch_worker is False :
        logging.info("Found launch template id and elastic worker tag in environment variable. Will start ET Management thread")
        thread = Thread(target = manage_elastic_instance, args=(args.worker_host_file, args.num_workers))
        thread.setDaemon(True)
        thread.start()

    if args.host_file is None or args.host_file == 'None':
      if args.cluster == 'yarn':
          from dmlc_tracker import yarn
          yarn.submit(args)
      elif args.cluster == 'local':
          from dmlc_tracker import local
          local.submit(args)
      elif args.cluster == 'sge':
          from dmlc_tracker import sge
          sge.submit(args)
      elif args.cluster == 'ssh' and args.launch_worker is True:
          from dmlc_tracker import ssh
          logging.info("dmlc_tracker ssh %s", args)
          ssh.submit(args)
      else:
          raise RuntimeError('Unknown submission cluster type %s' % args.cluster)
    else:
      if args.cluster == 'ssh':
          from dmlc_tracker import ssh
          logging.info("dmlc_tracker ssh %s", args)
          ssh.submit(args)
      elif args.cluster == 'mpi':
          from dmlc_tracker import mpi
          mpi.submit(args)
      else:
          raise RuntimeError('Unknown submission cluster type %s' % args.cluster)

def signal_handler(signal, frame):
    logging.info('Stop launcher')
    sys.exit(0)

if __name__ == '__main__':
    fmt = '%(asctime)s %(levelname)s %(message)s'
    logging.basicConfig(format=fmt, level=logging.INFO)
    signal.signal(signal.SIGINT, signal_handler)
    main()
