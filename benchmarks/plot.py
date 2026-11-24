#!/usr/bin/env python3
"""Plot actual business throughput and trial spread; requires matplotlib."""
import argparse
import json
from pathlib import Path
import statistics

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('run',type=Path)
    args=parser.parse_args()
    results=json.loads((args.run/'results.json').read_text())
    groups=[('Backend count',['proxy-1b-4w','proxy-3b-4w','proxy-5b-4w'],['1','3','5']),
            ('I/O workers',['workers-1','workers-2','proxy-3b-4w','workers-8','workers-0'],['1','2','4','8','auto']),
            ('Open wrk connections',['connections-10','proxy-3b-4w','connections-500','connections-1000'],['10','100','500','1000'])]
    fig,axes=plt.subplots(1,3,figsize=(13,4),sharey=True)
    for ax,(title,names,labels) in zip(axes,groups):
        cohorts=[[row['measurement'] for row in results if row['config']['name']==name] for name in names]
        medians=[statistics.median(row['successful_business_rps'] for row in cohort) for cohort in cohorts]
        lower=[median-min(row['successful_business_rps'] for row in cohort) for median,cohort in zip(medians,cohorts)]
        upper=[max(row['successful_business_rps'] for row in cohort)-median for median,cohort in zip(medians,cohorts)]
        ax.errorbar(range(len(names)),medians,yerr=[lower,upper],marker='o',capsize=4,color='#26718b',label='HTTP 200 RPS')
        ax.set_xticks(range(len(names)),labels)
        ax.set_xlabel(title)
        ax.grid(axis='y',alpha=.2)
        ax.set_ylim(bottom=0)
    axes[0].set_ylabel('Successful business responses / second')
    fig.suptitle('Local exploratory wrk trials: median and full trial range')
    fig.text(.5,.01,'3 × 3 s measured, 1 s warm-up; shared host; address-allocation errors/carryover confound comparisons. Not capacity estimates.',ha='center',fontsize=8)
    fig.tight_layout(rect=(0,.04,1,.93))
    fig.savefig(args.run/'throughput.png',dpi=160)
    fig.savefig(args.run/'throughput.svg')
    plt.close(fig)


if __name__=='__main__':
    main()
