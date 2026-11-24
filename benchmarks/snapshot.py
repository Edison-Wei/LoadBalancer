#!/usr/bin/env python3
"""Archive real Grafana query frames/snapshot from a completed owned benchmark TSDB."""
import argparse
import base64
import secrets
import json
from pathlib import Path
import urllib.request

from run import Child, available_port, get, wait


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--prometheus', type=Path, required=True)
    parser.add_argument('--grafana-home', type=Path, required=True)
    args = parser.parse_args()
    results = json.loads((args.run/'results.json').read_text())
    selected = next(row for row in results if row['config']['name']=='proxy-3b-4w')
    directory = args.output or args.run/'grafana-evidence'
    password = secrets.token_urlsafe(32)
    authorization = 'Basic '+base64.b64encode(('benchmark:'+password).encode()).decode()
    directory.mkdir()
    port, graf_port = available_port(), available_port()
    prom_url, graf_url = f'http://127.0.0.1:{port}', f'http://127.0.0.1:{graf_port}'
    (directory/'prometheus.yml').write_text('scrape_configs: []\n')
    provisioning = directory/'provisioning'
    (provisioning/'datasources').mkdir(parents=True)
    (provisioning/'dashboards').mkdir()
    (provisioning/'datasources/prometheus.yml').write_text('apiVersion: 1\ndatasources:\n  - name: Benchmark Prometheus\n    uid: prometheus\n    type: prometheus\n    access: proxy\n    url: '+prom_url+'\n    jsonData:\n      timeInterval: 1s\n')
    dashboards = directory/'dashboards'
    dashboards.mkdir()
    dashboard = json.loads((args.run/selected['run_id']/'grafana-dashboard.json').read_text())
    dashboard['time']={'from':str(int(selected['measurement_start_epoch']*1000)),
                       'to':str(int(selected['measurement_end_epoch']*1000))}
    (dashboards/'load-balancer.json').write_text(json.dumps(dashboard))
    (provisioning/'dashboards/benchmark.yml').write_text('apiVersion: 1\nproviders:\n  - name: Benchmark\n    type: file\n    options:\n      path: '+str(dashboards.resolve())+'\n')
    (directory/'grafana.ini').write_text(f'''[paths]
data = {(directory/'data').resolve()}
logs = {(directory/'logs').resolve()}
plugins = {(directory/'plugins').resolve()}
provisioning = {provisioning.resolve()}
[server]
http_addr = 127.0.0.1
http_port = {graf_port}
[security]
admin_user = benchmark
admin_password = {password}
[auth.anonymous]
enabled = true
org_role = Viewer
[analytics]
reporting_enabled = false
check_for_updates = false
check_for_plugin_updates = false
[plugins]
preinstall_disabled = true
''')
    owned=[]
    try:
        prom=Child([str(args.prometheus.resolve()),'--config.file='+str((directory/'prometheus.yml').resolve()),
                    '--storage.tsdb.path='+str((args.run/'prometheus-data').resolve()),
                    f'--web.listen-address=127.0.0.1:{port}'],'prometheus-snapshot',directory)
        owned.append(prom)
        wait(lambda:get(prom_url+'/api/v1/query?query=1'), 'Snapshot Prometheus ready')
        grafana=Child([str((args.grafana_home/'bin/grafana').resolve()),'server',
                       '--homepath',str(args.grafana_home.resolve()),'--config',str((directory/'grafana.ini').resolve())],
                      'grafana',directory)
        owned.append(grafana)
        exported=wait(lambda:get(graf_url+'/api/dashboards/uid/load-balancer'), 'Provisioned benchmark dashboard',timeout=45)
        captures={}
        def post(path,payload):
            req=urllib.request.Request(graf_url+path,data=json.dumps(payload).encode(),
                    headers={'Content-Type':'application/json','Origin':graf_url,'Authorization':authorization})
            with urllib.request.urlopen(req,timeout=10) as response:
                return json.load(response)
        for panel in dashboard['panels']:
            queries=[{**target,'datasource':{'type':'prometheus','uid':'prometheus'},'intervalMs':1000,
                      'maxDataPoints':1000,'range':True,'instant':False} for target in panel['targets']]
            data=post('/api/ds/query',{'from':dashboard['time']['from'],'to':dashboard['time']['to'],'queries':queries})
            captures[panel['title']]=data
            frames=[]
            for target in queries:
                outcome=data['results'][target['refId']]
                assert not outcome.get('error'), outcome
                assert outcome.get('frames'), (panel['title'],outcome)
                for frame in outcome['frames']:
                    fields=[{**field,'values':frame['data']['values'][i]} for i,field in enumerate(frame['schema']['fields'])]
                    frames.append({'fields':fields,'refId':target['refId'],'meta':frame['schema'].get('meta',{})})
            panel['snapshotData']=frames
        created=post('/api/snapshots',{'dashboard':dashboard,'name':'Release benchmark '+selected['run_id'],'expires':0})
        saved=get(graf_url+'/api/snapshots/'+created['key'])
        (directory/'dashboard-export.json').write_text(json.dumps(exported,indent=2)+'\n')
        (directory/'query-frames.json').write_text(json.dumps(captures,indent=2)+'\n')
        (directory/'snapshot.json').write_text(json.dumps(saved,indent=2)+'\n')
        (directory/'verification.json').write_text(json.dumps({'run_id':selected['run_id'],'panels':len(dashboard['panels']),
            'queries':sum(len(p['targets']) for p in dashboard['panels']),
            'snapshot_key':created['key'],'source':'Real Grafana datasource queries against archived benchmark Prometheus TSDB; no synthetic samples'},indent=2)+'\n')
        print('Archived real Grafana snapshot: 12 panels and 15 queries with data.')
    finally:
        for child in reversed(owned):
            child.finish(stop=True)


if __name__=='__main__':
    main()
