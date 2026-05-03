# Used only in github actions just to build the archlinux image for the standalone package

FROM archlinux:latest

# Initialize keyring + update + install essentials
RUN pacman -Syu --noconfirm \
    && pacman -S --noconfirm \
        git \
        base-devel \
        sudo \
        ca-certificates \
    && pacman -Scc --noconfirm

# Create a non-root user (some tools expect it)
RUN useradd -m builder \
    && echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

WORKDIR /home/builder
