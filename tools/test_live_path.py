#!/usr/bin/env python3
"""Host-only BACnet/Modbus test. Fault injection affects only the local proxy.

The ATS receives only the production poller's ordinary FC03 requests.
This does not flash, reboot, reconfigure or interrupt the ATS or an ESP board.
"""
import asyncio
import json
import os
from pathlib import Path
import sys
from bacpypes3.argparse import SimpleArgumentParser
from bacpypes3.app import Application
from bacpypes3.pdu import Address
from bacpypes3.primitivedata import ObjectIdentifier


async def run(args):
    args.output.mkdir(parents=True,exist_ok=False)
    app=Application.from_args(args)
    paused=False
    requests=0
    native=None
    report={'passed':False,'checks':{}}

    async def proxy(reader,writer):
        nonlocal requests
        remote=None
        try:
            req=await asyncio.wait_for(reader.readexactly(12),2)
            assert req[2:4]==b'\0\0' and req[4:6]==b'\0\6' and req[7]==3
            assert 1<=int.from_bytes(req[10:12],'big')<=50
            if paused:
                await asyncio.sleep(1.5)
                return
            requests+=1
            rr,remote=await asyncio.wait_for(asyncio.open_connection(args.ats,args.ats_port),2)
            remote.write(req); await remote.drain()
            header=await asyncio.wait_for(rr.readexactly(7),2)
            length=int.from_bytes(header[4:6],'big')
            assert 2<=length<=254
            body=await asyncio.wait_for(rr.readexactly(length-1),2)
            writer.write(header+body); await writer.drain()
        except (asyncio.TimeoutError,asyncio.IncompleteReadError,ConnectionError,OSError):
            pass
        finally:
            writer.close()
            if remote: remote.close()

    async def get_fault(subscription,wanted,seconds):
        end=asyncio.get_running_loop().time()+seconds
        while True:
            prop,value=await asyncio.wait_for(subscription.get_value(),end-asyncio.get_running_loop().time())
            if str(prop)=='status-flags' and ('fault' in str(value))==wanted:
                return str(value)

    server=await asyncio.start_server(proxy,'127.0.0.1',0)
    port=server.sockets[0].getsockname()[1]
    log=(args.output/'native.log').open('wb')
    target=Address('127.0.0.1:47819')
    try:
        native=await asyncio.create_subprocess_exec(str(args.native.resolve()),'127.0.0.1',str(port),
            '127.0.0.1','47819','90',stdout=log,stderr=asyncio.subprocess.STDOUT,
            env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
        await asyncio.sleep(12)
        check=await asyncio.create_subprocess_exec(sys.executable,
            str(Path(__file__).with_name('probe_gateway.py')),
            '--address','127.0.0.1:47820','--instance','3999999',
            '--target','127.0.0.1:47819','--expect-live','--check-write-denial',
            '--output',str(args.output/'bacnet-probe.json'),stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT)
        stdout,_=await asyncio.wait_for(check.communicate(),30)
        (args.output/'probe.log').write_bytes(stdout)
        assert check.returncode==0,stdout.decode(errors='replace')
        report['checks']['discovery_properties_cov']=json.loads((args.output/'bacnet-probe.json').read_text())['checks']
        obj=ObjectIdentifier('analog-input,1013')
        async with app.change_of_value(target,obj,issue_confirmed_notifications=True,lifetime=60) as sub:
            await get_fault(sub,False,5)
            paused=True
            await get_fault(sub,True,10)
            reliability=await app.read_property(target,obj,'reliability')
            assert str(reliability)=='communication-failure',str(reliability)
            value=await app.read_property(target,obj,'present-value')
            assert 400<float(value)<550,value
            report['checks']['proxy_loss']={'reliability':str(reliability),'retained_voltage':float(value),'confirmed_fault_cov':True}
            paused=False
            await get_fault(sub,False,20)
            reliability=await app.read_property(target,obj,'reliability')
            assert str(reliability)=='no-fault-detected',str(reliability)
            report['checks']['proxy_recovery']={'reliability':str(reliability),'confirmed_clear_cov':True}
        report['passed']=True
    except BaseException as exc:
        report['error']=repr(exc)
        raise
    finally:
        paused=False
        if native:
            if native.returncode is None: native.terminate()
            report['native_exit']=await asyncio.wait_for(native.wait(),5)
            if report['native_exit']!=0: report['passed']=False
        server.close(); await server.wait_closed()
        app.close(); log.close()
        report['forwarded_fc03_requests']=requests
        (args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps(report,indent=2))
    if not report['passed']: raise SystemExit(1)


if __name__=='__main__':
    parser=SimpleArgumentParser()
    parser.add_argument('--ats',required=True,help='Authorized ATS IPv4; reads only')
    parser.add_argument('--ats-port',type=int,default=502)
    parser.add_argument('--native',type=Path,default=Path('build-host/native_gateway'))
    parser.add_argument('--output',type=Path,required=True)
    asyncio.run(run(parser.parse_args()))
