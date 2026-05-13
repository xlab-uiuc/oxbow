#!/bin/bash

# 1000 secs
sudo sh -c "echo 1000000 > /sys/kernel/oxbow/oxbow_wq_timeout"
sudo cat /sys/kernel/oxbow/oxbow_wq_timeout
