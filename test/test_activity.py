#!/usr/bin/env python3
"""Controlled real C02 worker/frontend exchanges; no TeX installation required."""
import json, os, subprocess, sys
binary=sys.argv[1]
def run(activity):
    env={**os.environ,"TEXPRESSO_PROFILE":"1","TEXPRESSO_PROFILE_ACTIVITY":str(activity)}
    p=subprocess.run([binary],env=env,capture_output=True,text=True,timeout=15,check=True)
    rows=[]
    for line in p.stderr.splitlines():
        if line.startswith('[profile] '):
            assert len((line+'\n').encode())<512, line
            rows.append(json.loads(line[10:]))
    return p.stdout,rows
plain,off=run(0);output,rows=run(1)
assert output==plain and not off
stats=[r for r in rows if r['stage']=='native-activity']
workers=[r for r in stats if r['actor']=='worker']
def span(kind,tokens):
    values=[r for r in workers if r['kind']==kind and r['tokens']==tokens]
    assert values,(kind,tokens,workers)
    return max(values,key=lambda r:r['maxMs'])
cpu=next(r for r in rows if r['stage']=='native-slow' and r['actor']=='worker' and r['kind']=='work' and r['tokens']==101)
sleep=span('work',202);payload=span('read',303)
assert cpu['cpuMs']>=20 and cpu['query']==1,cpu
assert sleep['maxMs']>=40 and sleep['cpuAtMaxMs']<sleep['maxMs']/2,sleep
assert payload['maxMs']>=50 and payload['cpuAtMaxMs']<payload['maxMs']/2,payload
assert span('transaction',303)['maxMs']>=payload['maxMs']
park=max((r for r in workers if r['kind']=='park'),key=lambda r:r['maxMs'])
assert park['maxMs']>=100 and park['query']==5 and park['tag']=='CHLD',park
resumed=span('work',606)
assert resumed['query']==6 and resumed['pid']==park['pid'] and resumed['cpuAtMaxMs']>=10,resumed
assert resumed['maxAt']-resumed['maxMs']>=park['maxAt']-.001,(resumed,park)
child=span('work',505)
assert child['query']==1 and child['pid']==park['peerPid'] and child['pid']!=park['pid'],child
assert any(r['kind']=='fork' and r['pid']==park['pid'] for r in workers)
assert any(r['kind']=='poll' and r['peerPid']==child['pid'] for r in stats)
assert any(r['kind']=='response-flush' and r['peerPid']==park['pid'] for r in stats)
for r in stats:
    assert r['cpuMs']>=0 and r['ms']>=0,r
bound=[r for r in rows if r['actor']=='test-bound']
assert len([r for r in bound if r['stage']=='native-slow'])==4
summary=next(r for r in bound if r['stage']=='native-activity')
assert summary['count']==10 and summary['dropped']==6 and summary['maxMs']==29
print('PASS: default-off parity; CPU/sleep/payload attribution; fork parking/reset/query correlation; bounded records')
