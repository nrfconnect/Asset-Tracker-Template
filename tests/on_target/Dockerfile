FROM python:3.12.5-slim-bookworm

ENV DEBIAN_FRONTEND=noninteractive

ARG JLINK_VERSION=V794i
ARG GO_VERSION=1.20.5

WORKDIR /work
COPY requirements.txt /work/

RUN <<EOT
    apt-get -y update
    apt-get -y upgrade
    apt-get -y install wget libusb-1.0-0 git
    apt-get clean
    pip install --no-cache-dir -r requirements.txt
    rm -rf requirements.txt
EOT

# Install Segger J-Link software
RUN <<EOT
    wget -q --post-data 'accept_license_agreement=accepted' https://www.segger.com/downloads/jlink/JLink_Linux_${JLINK_VERSION}_x86_64.tgz
    mkdir -p /opt/SEGGER/JLink
    tar -xvf JLink_Linux_${JLINK_VERSION}_x86_64.tgz --strip-components=1 -C /opt/SEGGER/JLink
    rm JLink_Linux_${JLINK_VERSION}_x86_64.tgz
EOT
ENV PATH=$PATH:/opt/SEGGER/JLink

# Install nrfutil device
# Make nrfutil install in a shared location, because when used with GitHub
# Actions, the image will be launched with the home dir mounted from the local
# checkout.
ENV NRFUTIL_HOME=/usr/local/share/nrfutil
RUN <<EOT
    wget -q "https://files.nordicsemi.com/ui/api/v1/download?repoKey=swtools&path=external/nrfutil/executables/x86_64-unknown-linux-gnu/nrfutil&isNativeBrowsing=false" -O nrfutil
    mv nrfutil /usr/local/bin
    chmod +x /usr/local/bin/nrfutil
    nrfutil install device
    nrfutil install trace
EOT

# Install Go
RUN <<EOT
    wget -q https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz
    tar -C /usr/local -xzf go${GO_VERSION}.linux-amd64.tar.gz
    rm go${GO_VERSION}.linux-amd64.tar.gz
EOT

ENV PATH=$PATH:/usr/local/go/bin:/go/bin
ENV GOPATH=/go

# Install mcumgr
RUN <<EOT
    go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest
EOT
