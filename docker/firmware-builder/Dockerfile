# syntax=docker/dockerfile:1.7

ARG IDF_IMAGE=espressif/idf:v5.5.5
FROM ${IDF_IMAGE}

ARG FIRMWARE_SOURCE_REVISION=unknown
ARG FIRMWARE_SOURCE_URL=https://github.com/FoloToy/folo-ai-passport-xiaozhi

LABEL org.opencontainers.image.title="Folo AI Passport firmware builder" \
      org.opencontainers.image.description="Builds the Folo AI Passport ESP32-C3 firmware with a selected language and wake word" \
      org.opencontainers.image.source="${FIRMWARE_SOURCE_URL}" \
      org.opencontainers.image.revision="${FIRMWARE_SOURCE_REVISION}"

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

WORKDIR /opt/folo-ai-passport-videosearch
COPY . .

RUN chmod 0755 \
      docker/firmware-builder/entrypoint.sh \
      docker/firmware-builder/firmware_builder.py \
    && . "${IDF_PATH}/export.sh" >/dev/null \
    && idf.py --version \
    && python3 scripts/build.py --list-boards --json >/tmp/xiaozhi-boards.json \
    && python3 scripts/build.py --list-languages --json >/tmp/xiaozhi-languages.json

ENV FIRMWARE_SOURCE_DIR=/opt/folo-ai-passport-videosearch \
    FIRMWARE_OUTPUT_DIR=/output \
    FIRMWARE_SOURCE_REVISION=${FIRMWARE_SOURCE_REVISION} \
    PYTHONUNBUFFERED=1

ENTRYPOINT ["/opt/folo-ai-passport-videosearch/docker/firmware-builder/entrypoint.sh"]
