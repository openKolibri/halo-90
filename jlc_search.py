import urllib.request
import json
import urllib.parse
import sys

def search(keyword):
    url = f"https://jlcpcb.com/api/shoppingCart/smtGood/selectSmtComponentList"
    data = {
        "keyword": keyword,
        "componentAttributes": [],
        "searchSource": "search",
        "stockFlag": True
    }
    req = urllib.request.Request(url, data=json.dumps(data).encode("utf-8"), headers={
        "Content-Type": "application/json",
        "User-Agent": "Mozilla/5.0"
    })
    try:
        with urllib.request.urlopen(req) as response:
            res = json.loads(response.read().decode())
            if "data" in res and "list" in res["data"]:
                for item in res["data"]["list"][:10]:
                    print(f"LCSC: {item.get('componentCode')} | MFR: {item.get('componentModelEn')} | DESC: {item.get('describe')} | STOCK: {item.get('stockCount')} | PKG: {item.get('componentSpecificationEn')} | Basic: {item.get('baseLibrary')}")
            else:
                print("No results")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    search(sys.argv[1])
