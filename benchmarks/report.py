#!/usr/bin/env python3
"""Generate comparison tables from measured results, including error responses."""
import argparse
import csv
import json
from pathlib import Path
import statistics


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('run',type=Path)
    args=parser.parse_args()
    rows=json.loads((args.run/'results.json').read_text())
    summary=json.loads((args.run/'summary.json').read_text())
    metadata=json.loads((args.run/'metadata.json').read_text())
    columns=['run_id','backends','workers','connections','http_rps','business_rps','p50_ms','p95_ms','p99_ms',
             'http_errors','socket_errors','wrk_cpu_one_core_percent','proxy_cpu_lifetime_percent','proxy_peak_rss_bytes']
    with (args.run/'trials.csv').open('w',newline='') as output:
        writer=csv.DictWriter(output,fieldnames=columns)
        writer.writeheader()
        for row in rows:
            measure=row['measurement']
            generator=next(p for p in reversed(row['resources']) if p['exit_code']==0 and (p['name']=='wrk' or p['name'].startswith('wrk-recovery')))
            proxy=next((p for p in row['resources'] if p['name']=='proxy'),{})
            writer.writerow(dict(zip(columns,[row['run_id'],row['config']['backends'],row.get('actual_workers','origin'),
                row['config']['connections'],measure['rps'],measure['successful_business_rps'],measure['p50_ms'],
                measure['p95_ms'],measure['p99_ms'],sum(n for status,n in measure['statuses'].items() if status!='200'),
                sum(n for name,n in measure['errors'].items() if name!='status'),generator['cpu_percent_one_core'],
                proxy.get('cpu_percent_one_core'),proxy.get('max_rss_bytes')])))
    text=['# Release/wrk exploratory measurements','',
          f"Platform: {metadata['platform']}; reported logical CPUs: {metadata['cpu_count']}. "
          f"{metadata['trials']} trials/case, {metadata['duration_seconds']} s measured after "
          f"{metadata['warmup_seconds']} s warm-up; {metadata['threads']} wrk threads; "
          f"{metadata['cooldown_seconds']} s cooldown; order seed {metadata['seed']}.",'',
          '| Case | HTTP RPS median [min, max] | SD | Business RPS median | p50 / p95 / p99 ms | HTTP failure fraction median |',
          '|---|---:|---:|---:|---:|---:|']
    for row in summary:
        text.append(f"| {row['name']} | {row['rps_median']:.1f} [{row['rps_min']:.1f}, {row['rps_max']:.1f}] | "
                    f"{row['rps_stdev']:.1f} | {row['successful_business_rps_median']:.1f} | "
                    f"{row['p50_ms_median']:.3f} / {row['p95_ms_median']:.3f} / {row['p99_ms_median']:.3f} | "
                    f"{row['http_failure_fraction_median']:.4f} |")
    text += ['', 'Latency quantiles include all completed responses, including errors. '
        'Business RPS counts only HTTP 200 responses with validated backend identities. '
        'Each RPS spread/SD uses whole-trial values; median per-trial quantiles are not pooled quantiles.', '',
        'These are short, closed-loop measurements on a shared host, with response parsing, application '
        'logs and one-second Prometheus scraping enabled. They are not a production capacity claim. '
        'Address-allocation failures and carryover state can confound comparisons. Raw data, failed-start '
        'retries and lifecycle resource accounting are retained; missing host telemetry is not estimated.']
    (args.run/'comparison.md').write_text('\n'.join(text)+'\n')
    print('Generated measured comparison.md and trials.csv.')


if __name__=='__main__':
    main()
