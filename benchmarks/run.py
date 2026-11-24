#!/usr/bin/env python3
"""Owned-process Release/wrk comparisons. Measurements are never part of CTest."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import resource
import selectors
import signal
import socket
import statistics
import subprocess
import sys
import time
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/integration'))
from test_backend_process import check


def get(url):
    with urllib.request.urlopen(url, timeout=5) as response:
        return json.load(response)


def wait(action, message, timeout=15):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            result = action()
            if result:
                return result
        except (OSError, ValueError, KeyError) as error:
            last = str(error)
        time.sleep(.05)
    raise AssertionError(f'{message}: {last}')


class Child:
    def __init__(self, command, name, directory, ready=False):
        self.name, self.command = name, command
        self.started = time.monotonic()
        self.stderr = (directory / (name + '.stderr.log')).open('w')
        self.stdout = None if ready else (directory / (name + '.stdout.log')).open('w')
        self.process = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE if ready else self.stdout,
                                        stderr=self.stderr, text=True)
        self.code, self.usage, self.event = None, None, None
        self.directory = directory
        self.ready = ready

    def listening(self):
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            check(selector.select(15), f'{self.name}: no startup event')
            self.event = json.loads(self.process.stdout.readline())
        check(self.event['event'] == 'listening', 'Invalid startup event')
        return self.event['port']

    def poll(self):
        if self.code is None:
            pid, status, usage = os.wait4(self.process.pid, os.WNOHANG)
            if pid:
                self.code = os.waitstatus_to_exitcode(status)
                self.process.returncode = self.code
                elapsed = time.monotonic() - self.started
                self.usage = {'pid': pid, 'exit_code': self.code, 'wall_seconds': elapsed,
                    'user_seconds': usage.ru_utime, 'system_seconds': usage.ru_stime,
                    'cpu_percent_one_core': 100 * (usage.ru_utime + usage.ru_stime) / elapsed,
                    'max_rss_bytes': usage.ru_maxrss * (1 if sys.platform == 'darwin' else 1024),
                    'voluntary_context_switches': usage.ru_nvcsw, 'involuntary_context_switches': usage.ru_nivcsw}
        return self.code

    def finish(self, stop=False, expected=0):
        if stop and self.poll() is None:
            os.kill(self.process.pid, signal.SIGTERM)
        deadline = time.monotonic() + 10
        while self.poll() is None and time.monotonic() < deadline:
            time.sleep(.01)
        if self.code is None:
            os.kill(self.process.pid, signal.SIGKILL)
            wait(lambda: self.poll() is not None, self.name + ' reap')
        if self.ready:
            self.process.stdout.close()
        if self.stdout:
            self.stdout.close()
        self.stderr.close()
        check(self.code == expected, f'{self.name}: exit {self.code}, expected {expected}')
        return {'name': self.name, 'command': self.command, **self.usage}


def available_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def cases():
    result = [dict(name='direct', backends=1, workers=None, connections=100)]
    for n in (1, 3, 5):
        result.append(dict(name=f'proxy-{n}b-4w', backends=n, workers=4, connections=100))
    for workers in (1, 2, 8, 0):
        result.append(dict(name=f'workers-{workers}', backends=3, workers=workers, connections=100))
    for connections in (10, 500, 1000):
        result.append(dict(name=f'connections-{connections}', backends=3, workers=4, connections=connections))
    result.append(dict(name='quiescent-probes', backends=3, workers=4, connections=100, interval=60000))
    result.append(dict(name='controlled-crash', backends=3, workers=4, connections=100, failure=True))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=ROOT / 'build/release')
    parser.add_argument('--wrk', type=Path, required=True)
    parser.add_argument('--prometheus', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/benchmarks')
    parser.add_argument('--duration', type=int, default=30)
    parser.add_argument('--cooldown', type=int, default=2)
    parser.add_argument('--warmup', type=int, default=5)
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--threads', type=int, default=2)
    parser.add_argument('--seed', type=int, default=8)
    parser.add_argument('--case', action='append', help='Restrict to named cases')
    args = parser.parse_args()
    check(args.duration >= 2 and args.warmup >= 1 and args.trials >= 2 and args.threads >= 1 and args.cooldown >= 0,
          'Duration >=2, warmup >=1, trials >=2, threads >=1, cooldown >=0 required')
    cache = (args.build / 'CMakeCache.txt').read_text()
    check(re.search(r'^CMAKE_BUILD_TYPE:[^=]+=Release$', cache, re.M) and
          re.search(r'^LB_SANITIZER:[^=]+=none$', cache, re.M), 'Benchmarks require unsanitized Release build')
    for binary in (args.wrk, args.prometheus, args.build / 'load_balancer', args.build / 'test_backend'):
        check(binary.is_file() and os.access(binary, os.X_OK), f'Executable missing: {binary}')
    check(not args.output.exists(), 'Choose a new output directory to preserve previous measurements')
    args.output.mkdir(parents=True)
    (args.output / 'targets.json').write_text('[]\n')
    selected = [case for case in cases() if not args.case or case['name'] in args.case]
    check(selected and (not args.case or set(args.case) == {case['name'] for case in selected}), 'Unknown/empty case selection')
    check(all(case['connections'] % args.threads == 0 for case in selected),
          'Connection counts must divide evenly among wrk threads; use --threads 2 for the full matrix')
    schedule = []
    rng = random.Random(args.seed)
    for trial in range(args.trials):
        order = list(selected)
        rng.shuffle(order)
        schedule += [{**case, 'trial': trial + 1} for case in order]
    metadata = {'platform': platform.platform(), 'machine': platform.machine(), 'cpu_count': os.cpu_count(),
        'python': sys.version, 'git_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'git_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()),
        'seed': args.seed, 'duration_seconds': args.duration, 'warmup_seconds': args.warmup, 'trials': args.trials,
        'threads': args.threads, 'cooldown_seconds': args.cooldown, 'schedule': schedule,
        'fd_limits': list(resource.getrlimit(resource.RLIMIT_NOFILE)),
        'binaries': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                     [args.wrk, args.prometheus, args.build / 'load_balancer', args.build / 'test_backend']},
        'sysctl': subprocess.run(['sysctl', 'hw.model', 'hw.ncpu', 'hw.memsize', 'kern.maxfiles', 'kern.maxfilesperproc'],
                                  capture_output=True, text=True).stdout,
        'system_stats_limitations': 'Live ps/sysctl access may be denied; per-child wait4 rusage covers lifetime including warmup, with peak RSS, not host timeseries.',
        'workload': 'GET /request, no think time/pipelining, downstream keep-alive, fresh business upstream per request; response-hook enabled in every trial'}
    (args.output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
    prom_port = available_port()
    prom_url = f'http://127.0.0.1:{prom_port}'
    (args.output / 'prometheus.yml').write_text('global:\n  scrape_interval: 1s\n  scrape_timeout: 800ms\nscrape_configs:\n  - job_name: cpp-load-balancer\n    file_sd_configs:\n      - files: [' + json.dumps(str((args.output / 'targets.json').resolve())) + ']\n        refresh_interval: 1s\n')
    owned = []
    results = []
    monitor = Child([str(args.prometheus.resolve()), '--config.file=' + str((args.output / 'prometheus.yml').resolve()),
                     '--storage.tsdb.path=' + str((args.output / 'prometheus-data').resolve()),
                     f'--web.listen-address=127.0.0.1:{prom_port}'], 'prometheus', args.output)
    owned.append(monitor)
    def query(expression):
        data = get(prom_url + '/api/v1/query?' + urllib.parse.urlencode({'query': expression}))
        check(data['status'] == 'success', 'Prometheus query failed')
        return data['data']['result']
    try:
        wait(lambda: query('1'), 'Owned Prometheus ready')
        for config in schedule:
            run_id = f'{config["name"]}-t{config["trial"]}'
            directory = args.output / run_id
            directory.mkdir()
            current, resources, ports = [], [], []
            result = {'run_id': run_id, 'config': config, 'resources': resources, 'passed': False}
            (directory / 'result.json').write_text(json.dumps(result, indent=2))
            try:
                for n in range(config['backends']):
                    child = Child([str((args.build / 'test_backend').resolve()), '--id', chr(65+n), '--port', '0'], f'backend-{n}', directory, True)
                    current.append(child); ports.append(child.listening())
                proxy = None
                port = ports[0]
                if config['workers'] is not None:
                    flags = ['--port','0','--metrics-port','0','--workers',str(config['workers']),
                             '--health-interval-ms',str(config.get('interval',200)),
                             '--max-connections','1600','--max-inflight','1200','--max-upstream-connections','1200']
                    for n, endpoint in enumerate(ports):
                        flags += ['--backend',f'{chr(65+n)},127.0.0.1,{endpoint}']
                    proxy = Child([str((args.build / 'load_balancer').resolve()), *flags], 'proxy', directory, True)
                    current.append(proxy); port = proxy.listening()
                    result['actual_workers'] = proxy.event['workers']
                    result['eligible_at_startup'] = proxy.event['eligible']
                    result['metrics_port'] = proxy.event['metrics_port']
                    targets = [{'targets':[f'127.0.0.1:{proxy.event["metrics_port"]}'], 'labels':{'run_id': run_id}}]
                    temporary = args.output / 'targets.tmp'
                    temporary.write_text(json.dumps(targets)); temporary.replace(args.output / 'targets.json')
                    wait(lambda: any(float(row['value'][1]) == 1 for row in query(f'up{{run_id="{run_id}"}}')), 'Prometheus scrapes this trial')
                else:
                    (args.output / 'targets.json').write_text('[]\n')
                command = [str(args.wrk.resolve()), '-t',str(args.threads), '-c',str(config['connections']),
                           '--latency','--timeout','3s','-s',str(ROOT / 'benchmarks/report.lua')]
                def generator(duration, label):
                    for attempt in range(12):
                        name = label if attempt == 0 else f'{label}-retry-{attempt}'
                        child = Child([*command,'-d',f'{duration}s',f'http://127.0.0.1:{port}/request'],name,directory)
                        current.append(child)
                        deadline = time.monotonic()+duration+15
                        while child.poll() is None and time.monotonic()<deadline:
                            time.sleep(.02)
                        if child.code == 1 and "Can't assign requested address" in (directory/(name+'.stderr.log')).read_text():
                            resources.append(child.finish(expected=1)); current.remove(child)
                            result.setdefault('generator_start_retries',[]).append({'label':label,'attempt':attempt,'error':'EADDRNOTAVAIL'})
                            time.sleep(5)
                            continue
                        return child, name
                    raise AssertionError('wrk startup EADDRNOTAVAIL persists after twelve attempts')
                warm, warm_name = generator(args.warmup, 'warmup')
                resources.append(warm.finish()); current.remove(warm)
                wait(lambda: socket.create_connection(('127.0.0.1',port),timeout=3).close() or True, 'Generator TCP endpoint available', timeout=75)
                before = time.time()
                work = Child([*command,'-d',f'{args.duration}s',f'http://127.0.0.1:{port}/request'], 'wrk', directory)
                current.append(work)
                fault, recovered = None, False
                while work.poll() is None:
                    elapsed = time.time() - before
                    if config.get('failure') and fault is None and elapsed >= args.duration / 3:
                        victim = current[1]
                        os.kill(victim.process.pid, signal.SIGKILL)
                        resources.append(victim.finish(expected=-signal.SIGKILL)); current.remove(victim)
                        fault = {'timestamp':time.time(),'action':'kill','backend':'B','pid':victim.process.pid}
                    if fault and not recovered and elapsed >= args.duration * 2 / 3:
                        replacement = Child([str((args.build / 'test_backend').resolve()), '--id','B','--port',str(ports[1])], 'backend-B-restart', directory, True)
                        current.append(replacement); replacement.listening()
                        fault['restart_timestamp'] = time.time(); recovered = True
                    time.sleep(.02)
                if work.code == 1 and "Can't assign requested address" in (directory/'wrk.stderr.log').read_text():
                    resources.append(work.finish(expected=1)); current.remove(work)
                    result.setdefault('generator_start_retries',[]).append({'label':'wrk','attempt':0,'error':'EADDRNOTAVAIL'})
                    check(not config.get('failure'), 'Fault trial must not retry after injection')
                    before = time.time()
                    work, work_name = generator(args.duration,'wrk-recovery')
                else:
                    work_name = 'wrk'
                resources.append(work.finish()); current.remove(work)
                ended = time.time()
                result['measurement_start_epoch'] = before
                result['measurement_end_epoch'] = ended
                text = (directory / (work_name+'.stdout.log')).read_text()
                matches = re.findall(r'^LB_WRK_JSON (.+)$', text, re.M)
                check(len(matches) == 1, 'wrk did not emit exactly one structured result')
                measured = json.loads(matches[0]); result['measurement'] = measured; result['fault'] = fault
                check(measured['requests'] > 0 and measured['invalid_identities'] == 0, 'No requests or invalid identity')
                check(sum(measured['statuses'].values()) == measured['requests'], 'Response accounting mismatch')
                check(set(measured['backends']) <= {chr(65+n) for n in range(config['backends'])}, 'Wrong backend identity')
                measured['successful_business_rps'] = measured['statuses'].get('200',0) / measured['duration_seconds']
                measured['http_failure_fraction'] = 1 - measured['statuses'].get('200',0) / measured['requests']
                check(set(measured['statuses']) <= {'200','502','503','504'}, 'Unexpected HTTP classification')
                check(sum(measured['backends'].values()) == measured['statuses'].get('200',0), 'Business identity accounting mismatch')
                if not config.get('failure'):
                    result['all_backends_observed_in_business_responses'] = len(measured['backends']) == config['backends']
                if proxy:
                    if fault:
                        wait(lambda: sum(float(row['value'][1]) == 1 for row in query(f'lb_backend_healthy{{run_id="{run_id}"}}')) == config['backends'],
                             'Failure trial recovery')
                    raw = wait(lambda: urllib.request.urlopen(f'http://127.0.0.1:{proxy.event["metrics_port"]}/metrics', timeout=5).read(), 'Post-trial metrics connection', timeout=45)
                    (directory / 'metrics.txt').write_bytes(raw)
                    dashboard = json.loads((ROOT / 'monitoring/grafana/dashboards/load-balancer.json').read_text())
                    snapshot = {}
                    for panel in dashboard['panels']:
                        for target in panel['targets']:
                            expr = target['expr'].replace('job="cpp-load-balancer"', f'job="cpp-load-balancer",run_id="{run_id}"')
                            target['expr'] = expr
                            data = wait(lambda: get(prom_url+'/api/v1/query_range?'+urllib.parse.urlencode({'query':expr,'start':before,'end':ended,'step':'1s'})), 'Prometheus range snapshot', timeout=45)
                            check(data['status']=='success','Snapshot query failed')
                            snapshot[panel['title']+'/'+target['refId']] = data
                    (directory / 'prometheus-snapshot.json').write_text(json.dumps(snapshot,indent=2))
                    (directory / 'grafana-dashboard.json').write_text(json.dumps(dashboard,indent=2))
                result['passed'] = True
            except BaseException as error:
                result['failure'] = f'{type(error).__name__}: {error}'
                raise
            finally:
                for child in reversed(current):
                    try:
                        resources.append(child.finish(stop=True))
                    except BaseException as error:
                        result.setdefault('cleanup_errors',[]).append(str(error)); result['passed'] = False
                (directory / 'result.json').write_text(json.dumps(result,indent=2)+'\n')
                for path in directory.glob('*.stderr.log'):
                    with path.open('rb') as source, gzip.open(str(path)+'.gz','wb') as target:
                        for block in iter(lambda:source.read(1048576), b''): target.write(block)
                    path.unlink()
                results.append(result)
                (args.output / 'results.json').write_text(json.dumps(results,indent=2)+'\n')
                print(run_id, result['passed'], result.get('measurement',{}).get('rps'),flush=True)
            check(result['passed'], 'Benchmark cleanup failed')
            time.sleep(args.cooldown)
        summary = []
        for config in selected:
            rows = [row for row in results if row['config']['name']==config['name']]
            rps = [row['measurement']['rps'] for row in rows]
            summary.append({'name':config['name'],'trials':len(rows),'rps_median':statistics.median(rps),
                'rps_min':min(rps),'rps_max':max(rps),'rps_stdev':statistics.stdev(rps),
                **{key+'_median':statistics.median(row['measurement'][key] for row in rows) for key in ['p50_ms','p95_ms','p99_ms','successful_business_rps','http_failure_fraction']}})
        (args.output / 'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    finally:
        for child in reversed(owned):
            usage = child.finish(stop=True)
            (args.output / (child.name+'-resource.json')).write_text(json.dumps(usage,indent=2)+'\n')


if __name__ == '__main__':
    main()
