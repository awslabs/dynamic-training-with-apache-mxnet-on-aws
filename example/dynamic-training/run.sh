# Copyright 2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of this
# software and associated documentation files (the "Software"), to deal in the Software
# without restriction, including without limitation the rights to use, copy, modify,
# merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
# PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

/home/ubuntu/.local/lib/python3.6/site-packages/mxnet/tools/launch.py -n $DEEPLEARNING_WORKERS_COUNT -H $DEEPLEARNING_WORKERS_PATH --elastic-training-enabled True python /myEFSvolume/train_resnet.py --use-rec --batch-size 4096 --dtype float16 --num-data-workers 40 --num-epochs 90 --gpus 0,1,2,3,4,5,6,7 --lr 1.6 --lr-mode poly --warmup-epochs 5 --last-gamma --mode symbolic --model resnet50_v1b --kvstore dist_sync_device --rec-train /media/ramdisk/pass-through/train-passthrough.rec --rec-train-idx /media/ramdisk/pass-through/train-passthrough.idx --rec-val /media/ramdisk/pass-through/val-passthrough.rec --rec-val-idx /media/ramdisk/pass-through/val-passthrough.idx --save-frequency 1000 --warmup-lr 0.001
