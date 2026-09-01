FROM ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517

ARG DEBIAN_FRONTEND=noninteractive
ARG GO_VERSION=1.26.4
ARG GO_SHA256=1153d3d50e0ac764b447adfe05c2bcf08e889d42a02e0fe0259bd47f6733ad7f

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
      build-essential \
      ca-certificates \
      ccache \
      clang \
      clang-format \
      clang-tidy \
      cmake \
      curl \
      git \
      make \
      ninja-build \
      python3.12 \
      python3.12-dev \
      python3.12-venv \
      shellcheck \
    && rm -rf /var/lib/apt/lists/*

RUN curl --fail --location --silent --show-error \
      --output /tmp/go.tar.gz \
      "https://go.dev/dl/go${GO_VERSION}.linux-amd64.tar.gz" \
    && echo "${GO_SHA256}  /tmp/go.tar.gz" | sha256sum --check --strict \
    && tar --directory /usr/local --extract --gzip --file /tmp/go.tar.gz \
    && rm /tmp/go.tar.gz

ENV PATH="/usr/local/go/bin:${PATH}"
ENV AEGIS_SKIP_MODULES=1
ENV PYTHONHASHSEED=20260828
ENV AEGIS_TEST_SEED=20260828

RUN useradd --create-home --uid 1000 --shell /bin/bash aegis

USER aegis
WORKDIR /workspace/aegis-mx

CMD ["sleep", "infinity"]
