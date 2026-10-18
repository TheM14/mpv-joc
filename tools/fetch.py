import http.client, sys
from urllib.parse import urlparse

url, out = sys.argv[1], sys.argv[2]
p = urlparse(url)
conn = http.client.HTTPConnection(p.hostname, p.port, timeout=120)
conn.request("GET", p.path, headers={"Range": "bytes=0-"})
resp = conn.getresponse()
data = resp.read()
with open(out, "wb") as handle:
    handle.write(data)
print("status", resp.status, "bytes", len(data), "content-range", resp.getheader("Content-Range"))
