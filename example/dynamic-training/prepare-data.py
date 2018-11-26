import subprocess
import os

bashCommand1 = 'sudo mkdir -p /media/ramdisk'
bashCommand2 = 'sudo mount -t tmpfs -o size=200G tmpfs /media/ramdisk'

process = subprocess.Popen(bashCommand1.split(), stdout=subprocess.PIPE)
output, error = process.communicate()

process = subprocess.Popen(bashCommand2.split(), stdout=subprocess.PIPE)
output, error = process.communicate()

os.environ['AWS_ACCESS_KEY_ID'] = 'YOUR_AWS_ACCESS_KEY_ID'
os.environ['AWS_SECRET_ACCESS_KEY'] = 'YOUR_AWS_SECRET_ACCESS_KEY'
os.environ['AWS_SESSION_TOKEN'] = 'YOUR_AWS_SESSION_TOKEN'

bashCommand0 = 'aws s3 sync s3://YOUR_DATASET/ /media/ramdisk/pass-through/'

subprocess.check_call(bashCommand0, shell=True, env=os.environ)

bashCommand0 = 'pip install gluoncv --user'
subprocess.check_call(bashCommand0, shell=True)
