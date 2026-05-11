FROM python:3.12-alpine

WORKDIR /app
COPY requirements.txt .
RUN apk add --no-cache util-linux \
  && pip install --no-cache-dir -r requirements.txt

COPY hmi_server.py .
COPY static ./static

ENV DATA_DIR=/data
EXPOSE 80
CMD ["python", "-u", "hmi_server.py"]
