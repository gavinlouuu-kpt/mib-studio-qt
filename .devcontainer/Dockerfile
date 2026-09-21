# Dev/agent container for MIB Studio Qt. The apt list is env/apt-packages.txt
# (the only package list in the repo); SECTIONS picks which parts to install.
# Ubuntu 24.04's Qt 6.4.2 configures, builds and passes the backend CTest
# suite (docs/howto/linux-build.md); Conan Qt 6.7.3 stays the release toolchain.
FROM ubuntu:24.04
ARG SECTIONS=base,backend,frontend
ENV DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC

COPY env/apt-packages.txt /tmp/apt-packages.txt
RUN set -eux; \
    pkgs="$(awk -v want=",${SECTIONS}," ' \
        /^# section:/ { sec=$3; next } \
        /^#/ || /^[[:space:]]*$/ { next } \
        index(want, "," sec ",") { print $1 } \
      ' /tmp/apt-packages.txt | tr '\n' ' ')"; \
    apt-get update; \
    apt-get install -y --no-install-recommends ${pkgs} sudo less nano; \
    rm -rf /var/lib/apt/lists/*

# Conan 2 + NumPy on PATH for every user (env/requirements-build.txt).
COPY env/requirements-build.txt /tmp/requirements-build.txt
RUN python3 -m venv /opt/venv && /opt/venv/bin/pip install --no-cache-dir -r /tmp/requirements-build.txt
ENV PATH=/opt/venv/bin:$PATH

# Work as the image's unprivileged user (uid 1000) with passwordless sudo so
# scripts/bootstrap.sh can still apt-get if a section is added later.
RUN echo 'ubuntu ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/ubuntu && chmod 0440 /etc/sudoers.d/ubuntu
USER ubuntu
WORKDIR /workspaces/mib-studio-qt
