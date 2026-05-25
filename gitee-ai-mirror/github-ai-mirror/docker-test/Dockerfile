FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    g++ cmake git openssh-server sudo libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Setup SSH
RUN mkdir -p /run/sshd && ssh-keygen -A

# Create test user (main user)
RUN useradd -m -s /bin/bash maxx && echo "maxx: maxx" | chpasswd

# Create project directory structure
RUN mkdir -p /home/maxx/dev/listen_msg_from_tg

# Copy source
COPY . /opt/ai-mirror-src/

# Build
WORKDIR /opt/ai-mirror-src
RUN cmake -B build-test -DCMAKE_BUILD_TYPE=Debug . && cmake --build build-test --target ai-mirror -j$(nproc)

# Install binary (no sudo needed in docker)
RUN install -d /usr/local/bin && install -m 0755 build-test/bin/ai-mirror /usr/local/bin/ai-mirror-bin

# Setup am.sh
RUN mkdir -p /etc/profile.d && cp profile/am.sh /etc/profile.d/am.sh

# Setup config
RUN mkdir -p /etc/ai-mirror
RUN echo '[user]\nprefix = "imaxx_"\nshell = "/bin/bash"\n' > /etc/ai-mirror/config.toml

WORKDIR /home/maxx/dev
