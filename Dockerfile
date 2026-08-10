FROM alpine:latest

SHELL ["sh", "-xeu", "-c"]

RUN apk update
RUN apk add \
    build-base \
    musl-dev \
    linux-headers \
    clang \
    llvm \
    lld \
    make \
    mingw-w64-gcc \
    git \
    cmake \
    patch \
    python3 \
    bash \
    curl \
    xz \
    ca-certificates \
    libxml2-dev \
    openssl-dev \
    bsd-compat-headers \
    fts-dev

ARG MAC_SDK_URL="https://github.com/joseluisq/macosx-sdks/releases/download/11.3/MacOSX11.3.sdk.tar.xz"

RUN git clone --depth=1 'https://github.com/tpoechtrager/osxcross' '/opt/osxcross' && \
    cd '/opt/osxcross' && \
    wget -nc "$MAC_SDK_URL" && \
    mv *.xz 'tarballs/' && \
    UNATTENDED=1 OSX_VERSION_MIN=11.0 ENABLE_ARCHS=arm64 ./build.sh

ENV PATH="/opt/osxcross/target/bin:$PATH"

ENV KOSH_TARGETS="x86_64-linux-musl aarch64-apple-darwin"

RUN git config --global --add safe.directory '*'

WORKDIR /src
