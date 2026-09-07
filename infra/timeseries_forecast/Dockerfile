FROM python:3.12.12-slim-bookworm@sha256:593bd06efe90efa80dc4eee3948be7c0fde4134606dd40d8dd8dbcade98e669c

ARG SOURCE_DATE_EPOCH=0

RUN python -m venv /opt/aegis/venv

COPY --chown=65532:65532 python/requirements-service.lock /opt/aegis/requirements-service.lock
RUN /opt/aegis/venv/bin/pip install \
      --disable-pip-version-check \
      --no-cache-dir \
      --require-hashes \
      --requirement /opt/aegis/requirements-service.lock

COPY --chown=65532:65532 python/intelligence/aegis /opt/aegis/python/aegis
COPY --chown=65532:65532 python/intelligence/aegis_mx_intelligence /opt/aegis/python/aegis_mx_intelligence
COPY --chown=65532:65532 python/model_serving/aegis_mx_timeseries /opt/aegis/python/aegis_mx_timeseries
COPY --chown=65532:65532 tools/timeseries_service_entry.py /opt/aegis/timeseries_service_entry.py

ENV AEGIS_TIMESERIES_HOST=127.0.0.1
ENV AEGIS_TIMESERIES_PORT=8080
ENV AEGIS_TIMESERIES_ROLE=timeseries-forecast-api
ENV AEGIS_TLS_SECRET_ROOT=/var/run/aegis-mx/tls
ENV AEGIS_SERVICE_IDENTITY=""
ENV AEGIS_TLS_CLIENT_BINDINGS=""
ENV PYTHONDONTWRITEBYTECODE=1
ENV PYTHONHASHSEED=20260828
ENV PYTHONPATH=/opt/aegis/python

LABEL org.opencontainers.image.title="Aegis-MX time-series forecast service" \
      org.opencontainers.image.version="0.2.0" \
      io.aegis-mx.live-trading-capable="false"

USER 65532:65532
EXPOSE 8080
ENTRYPOINT ["/opt/aegis/venv/bin/python", "/opt/aegis/timeseries_service_entry.py"]
