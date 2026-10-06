ARG RUNTIME_BASE_IMAGE
FROM ${RUNTIME_BASE_IMAGE}

ARG TRADE_NGIN_GIT_SHA
ARG TRADE_NGIN_GIT_SHA_FULL
ARG APACHE_ARROW_APT_SOURCE_SHA256
ARG TRADE_NGIN_BOOTSTRAP_APT_PACKAGES
ARG TRADE_NGIN_RUNTIME_APT_PACKAGES
LABEL org.opencontainers.image.revision="${TRADE_NGIN_GIT_SHA_FULL}" \
      org.algogators.trade-ngin.short-sha="${TRADE_NGIN_GIT_SHA}"

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=America/New_York \
    LD_LIBRARY_PATH=/usr/local/lib:/app/build/bin/Release:/app/lib

# Runtime dependencies only. Compilation and tests happen once in the pinned CI
# toolchain; this image packages those exact bytes and never edits source code.
RUN test "${#APACHE_ARROW_APT_SOURCE_SHA256}" -eq 64 \
    && printf '%s\n' "${APACHE_ARROW_APT_SOURCE_SHA256}" | grep -Eq '^[0-9a-f]+$' \
    && test -n "${TRADE_NGIN_BOOTSTRAP_APT_PACKAGES}" \
    && test -n "${TRADE_NGIN_RUNTIME_APT_PACKAGES}" \
    && for package in ${TRADE_NGIN_BOOTSTRAP_APT_PACKAGES} ${TRADE_NGIN_RUNTIME_APT_PACKAGES}; do \
         case "${package}" in *=?*) ;; *) echo "package_missing_exact_version: ${package}" >&2; exit 1 ;; esac; \
       done \
    && apt-get update && apt-get install -y --no-install-recommends \
        ${TRADE_NGIN_BOOTSTRAP_APT_PACKAGES} \
    && wget -qO /tmp/apache-arrow-apt-source.deb \
        "https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-$(lsb_release -cs).deb" \
    && printf '%s  %s\n' "${APACHE_ARROW_APT_SOURCE_SHA256}" /tmp/apache-arrow-apt-source.deb \
        | sha256sum -c - \
    && apt-get install -y --no-install-recommends /tmp/apache-arrow-apt-source.deb \
    && rm /tmp/apache-arrow-apt-source.deb \
    && apt-get update && apt-get install -y --no-install-recommends \
        ${TRADE_NGIN_RUNTIME_APT_PACKAGES} \
    && apt-get purge -y --auto-remove gnupg lsb-release \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY build/qt-prod/bin/Release/ /app/build/bin/Release/
COPY build/qt-prod/qt-evaluator-bundle/ /app/build/qt-evaluator-bundle/
COPY build/qt-prod/live-config-validator-bundle/ /app/build/live-config-validator-bundle/
COPY build/qt-prod/release-files.sha256 /app/build/release-files.sha256
COPY config_template/ /app/config_template/
COPY scripts/ /app/scripts/
COPY live_portfolio.cron /etc/cron.d/live_portfolio

RUN test "${#TRADE_NGIN_GIT_SHA}" -ge 7 \
    && test "${#TRADE_NGIN_GIT_SHA}" -le 40 \
    && printf '%s\n' "${TRADE_NGIN_GIT_SHA}" | grep -Eq '^[0-9a-f]+$' \
    || { echo "release_identity_invalid: short SHA" >&2; exit 1; } \
    && test "${#TRADE_NGIN_GIT_SHA_FULL}" -eq 40 \
    && printf '%s\n' "${TRADE_NGIN_GIT_SHA_FULL}" | grep -Eq '^[0-9a-f]+$' \
    || { echo "release_identity_invalid: full SHA" >&2; exit 1; } \
    && test "${TRADE_NGIN_GIT_SHA_FULL#${TRADE_NGIN_GIT_SHA}}" != "${TRADE_NGIN_GIT_SHA_FULL}" \
    && test -x /app/build/bin/Release/live_portfolio_conservative \
    && test -x /app/build/bin/Release/qt_evaluator \
    && test -x /app/build/bin/Release/live_config_validate \
    && test -f /app/build/live-config-validator-bundle/live_config_validator_manifest.json \
    && test -x /app/build/bin/Release/qt_desk_worker \
    && test -f /app/build/qt-evaluator-bundle/qt_evaluator_manifest.json \
    && (cd /app/build && sha256sum -c release-files.sha256) \
    && chmod 0644 /etc/cron.d/live_portfolio \
    && crontab /etc/cron.d/live_portfolio \
    && chmod 0755 /app/scripts/run_live_portfolio.sh /app/scripts/docker-entrypoint.sh

HEALTHCHECK --interval=5m --timeout=10s --start-period=30s --retries=3 \
    CMD pgrep -x cron > /dev/null || exit 1

ENTRYPOINT ["/app/scripts/docker-entrypoint.sh"]
