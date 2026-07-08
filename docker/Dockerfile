# dicomq in a container.
#
# The build stage compiles DCMTK 3.7.0 from source — Ubuntu's apt package
# (3.6.7) predates the CVE-2025-14607 fix that CMakeLists.txt enforces as
# a floor — then dicomq against it. DCMTK is linked statically, so the
# runtime stage carries only the eight dicomq binaries plus OpenSSL and
# zlib.
#
# Build from the repository root (the compose file in this directory does):
#   docker build -f docker/Dockerfile -t dicomq .

FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates cmake g++ git make libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# The same trimmed module build as .github/workflows/linux.yml — only the
# libraries dicomq links. Keep the version and module list in step with
# that workflow (its comments explain why BUILD_APPS=OFF alone is not
# enough).
ARG DCMTK_VERSION=3.7.0
ARG DCMTK_MODULES="ofstd;oflog;oficonv;dcmdata;dcmimgle;dcmimage;dcmjpeg;dcmjpls;dcmnet;dcmtls"
RUN git clone --depth 1 --branch "DCMTK-${DCMTK_VERSION}" \
      https://github.com/DCMTK/dcmtk.git /tmp/dcmtk \
 && cmake -S /tmp/dcmtk -B /tmp/dcmtk/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_APPS=OFF \
      -DDCMTK_MODULES="${DCMTK_MODULES}" \
      -DDCMTK_WITH_OPENSSL=ON \
      -DDCMTK_WITH_ZLIB=ON \
      -DDCMTK_WITH_PNG=OFF \
      -DDCMTK_WITH_TIFF=OFF \
      -DCMAKE_INSTALL_PREFIX=/opt/dcmtk \
 && cmake --build /tmp/dcmtk/build --parallel \
 && cmake --install /tmp/dcmtk/build \
 && rm -rf /tmp/dcmtk

COPY . /src
RUN cmake -S /src -B /src/build -DCMAKE_PREFIX_PATH=/opt/dcmtk \
 && cmake --build /src/build --parallel \
 && cmake --install /src/build --prefix /opt/dicomq

FROM ubuntu:24.04

# libssl for dcmtls (TLS support), zlib for the deflated transfer syntax;
# DCMTK itself is statically linked into the binaries
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3t64 zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/dicomq/bin/ /usr/local/bin/
COPY docker/entrypoint.sh /usr/local/bin/dicomq-entrypoint

ENV DICOMQ_SPOOL=/var/spool/dicomq

# the entrypoint creates the spool skeleton (idempotently) and execs the
# requested program; the default command is the small-site listener
ENTRYPOINT ["dicomq-entrypoint"]
CMD ["dicomq-recv", "--listen", "11112"]
