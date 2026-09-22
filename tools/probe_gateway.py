#!/usr/bin/env python3
"""Independent BACpypes3 discovery/property/COV probe; no Modbus writes."""
import asyncio
import datetime
import json
from pathlib import Path
from bacpypes3.argparse import SimpleArgumentParser
from bacpypes3.app import Application
from bacpypes3.apdu import ErrorRejectAbortNack
from bacpypes3.basetypes import ErrorType
from bacpypes3.pdu import Address
from bacpypes3.primitivedata import ObjectIdentifier


async def run(args):
    app = Application.from_args(args)
    target = Address(args.target)
    device = ObjectIdentifier(('device',args.device))
    result = {'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'target':args.target,'checks':{},'properties':[]}
    try:
        found = await app.who_is(args.device,args.device,address=target,timeout=2)
        assert any(item.iAmDeviceIdentifier==device for item in found), 'No directed I-Am'
        result['checks']['directed_discovery']=True
        found = await app.who_is(args.device+1,args.device+1,address=target,timeout=.5)
        assert not found, 'Who-Is range ignored'
        result['checks']['discovery_range']=True
        objects = await app.read_property(target,device,'object-list')
        count = await app.read_property(target,device,'object-list',array_index=0)
        assert count==len(objects)==172, (count,len(objects))
        assert len(set(str(obj) for obj in objects))==len(objects)
        result['checks']['object_count']=count
        for i,obj in enumerate(objects,1):
            indexed=await app.read_property(target,device,'object-list',array_index=i)
            assert indexed==obj, (i,indexed,obj)
        result['checks']['indexed_object_list']=len(objects)
        names=[]
        for obj in objects:
            typ=int(obj[0])
            props=['object-identifier','object-type','object-name','description','property-list']
            if typ in (0,3,13,40):
                props+=['present-value','status-flags','reliability','out-of-service']
            if typ==0: props+=['units']
            if typ==13: props+=['number-of-states','state-text']
            reply=await app.read_property_multiple(target,[obj,props])
            assert len(reply)==len(props),(obj,len(reply),len(props))
            for ident,prop,index,value in reply:
                assert not isinstance(value,ErrorType),(ident,prop,str(value))
                result['properties'].append({'object':str(ident),'property':str(prop),'value':str(value)})
                if str(prop)=='object-name': names.append(str(value).casefold())
        assert len(set(names))==len(objects),'Duplicate object names'
        result['checks']['rpm_properties']=len(result['properties'])
        if args.expect_live:
            voltage=await app.read_property(target,'analog-input,1013','present-value')
            reliability=await app.read_property(target,'analog-input,1013','reliability')
            assert 400<float(voltage)<550,voltage
            assert str(reliability)=='no-fault-detected',str(reliability)
            result['checks']['live_normal_voltage']=float(voltage)
            current_quality=await app.read_property(target,'analog-input,1021','reliability')
            assert str(current_quality)!='no-fault-detected','Unqualified current advertised healthy'
            result['checks']['unverified_current_flagged']=str(current_quality)
        cov=[]
        for obj,confirmed in [('analog-input,1013',True),('binary-input,1005',False),
                              ('multi-state-input,1002',True),('characterstring-value,1137',True)]:
            notifications=[]
            async with app.change_of_value(target,ObjectIdentifier(obj),
                    issue_confirmed_notifications=confirmed,lifetime=30) as subscription:
                for _ in range(2):
                    prop,value=await asyncio.wait_for(subscription.get_value(),5)
                    notifications.append({'property':str(prop),'value':str(value)})
                assert any(n['property']=='present-value' for n in notifications)
                assert any(n['property']=='status-flags' for n in notifications)
            cov.append({'object':obj,'confirmed':confirmed,'notifications':notifications})
        result['checks']['cov']=cov
        if args.check_write_denial:
            value=await app.read_property(target,'analog-input,1013','present-value')
            try:
                await app.write_property(target,'analog-input,1013','present-value',value)
                raise AssertionError('Gateway accepted a read-only point write')
            except ErrorRejectAbortNack as err:
                assert 'write-access-denied' in str(err),str(err)
            result['checks']['read_only_write_denied']=True
        result['passed']=True
    except BaseException as exc:
        result['passed']=False
        result['error']=repr(exc)
        raise
    finally:
        app.close()
        args.output.parent.mkdir(parents=True,exist_ok=True)
        args.output.write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps({k:v for k,v in result.items() if k!='properties'},indent=2))


if __name__=='__main__':
    p=SimpleArgumentParser()
    p.add_argument('--target',required=True)
    p.add_argument('--device',type=int,default=75181)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--expect-live',action='store_true')
    p.add_argument('--check-write-denial',action='store_true',help='Same-value denied-write test; use only on the intended gateway')
    asyncio.run(run(p.parse_args()))
