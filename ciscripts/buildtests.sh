#!/bin/bash
set -e
sudo docker build --build-arg CI_BUILD=1 --build-arg BUILD_STEP=1 -t=steemit/hive:tests .
sudo docker run -v $WORKSPACE:/var/jenkins steemit/hive:tests cp -r /var/cobertura /var/jenkins
# make docker cleanup after itself and delete all exited containers
sudo docker rm -v $(docker ps -a -q -f status=exited) || true