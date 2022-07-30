FROM ubuntu:20.04

RUN apt update && apt -y install build-essential git iproute2 iputils-ping netcat-openbsd