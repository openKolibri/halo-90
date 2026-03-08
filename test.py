import urllib.request
import json
import ssl

ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE

# using their open parts lib API endpoint for search
url = "https://jlcpcb.com/api/partLibrary/searchComponentInfo?keyword=0402+red+led&baseLibrary=true"
req = urllib.request.Request(
    url,
    headers={"User-Agent": "Mozilla/5.0"}
)

try:
    with urllib.request.urlopen(req, context=ctx) as r:
        data = json.loads(r.read())
        if data.get("code") == 200:
            for x in data.get('data', {}).get('list', [])[:5]:
                print(f"LCSC: {x.get('lcscId')} | DESC: {x.get('describe')} | STOCK: {x.get('stockCount')} | Basic: {x.get('baseLibrary')} | Price: {x.get('componentPrice')}")
        else:
             print("Raw error:", data)
except Exception as e:
    print(e)
