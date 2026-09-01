FROM ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
      ca-certificates \
      python3.12 \
      python3.12-venv \
    && rm -rf /var/lib/apt/lists/* \
    && python3.12 -m venv /opt/aegis/venv

COPY python/requirements-service.lock /opt/aegis/requirements-service.lock
RUN /opt/aegis/venv/bin/pip install \
      --disable-pip-version-check \
      --no-cache-dir \
      --require-hashes \
      --requirement /opt/aegis/requirements-service.lock

COPY python/intelligence/aegis /opt/aegis/python/aegis
COPY python/intelligence/aegis_mx_intelligence /opt/aegis/python/aegis_mx_intelligence
COPY python/model_serving/aegis_mx_timeseries /opt/aegis/python/aegis_mx_timeseries
COPY tools/timeseries_service_entry.py /opt/aegis/timeseries_service_entry.py

RUN useradd --no-create-home --uid 65532 --shell /usr/sbin/nologin aegis \
    && chown --recursive 65532:65532 /opt/aegis

ENV AEGIS_TIMESERIES_HOST=127.0.0.1
ENV AEGIS_TIMESERIES_PORT=8080
ENV AEGIS_TIMESERIES_ROLE=timeseries-forecast-api
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONHASHSEED=20260828
ENV PYTHONPATH=/opt/aegis/python

LABEL org.opencontainers.image.title="Aegis-MX time-series forecast service" \
      org.opencontainers.image.version="0.2.0" \
      io.aegis-mx.live-trading-capable="false"

USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/opt/aegis/venv/bin/python", "/opt/aegis/timeseries_service_entry.py"]
