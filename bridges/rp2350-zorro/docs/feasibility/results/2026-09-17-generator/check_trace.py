"""Independent W0 observation oracle: sigrok raw channels D0..D3 and D7."""
import sys, zipfile, json
from pathlib import Path
p=Path(sys.argv[1])
with zipfile.ZipFile(p) as z:
    metadata=z.read('metadata').decode()
    assert 'samplerate=1 MHz' in metadata and 'unitsize=1' in metadata, metadata
    names=sorted((n for n in z.namelist() if n.startswith('logic-1-')), key=lambda n:int(n.rsplit('-',1)[1]))
    data=b''.join(z.read(n) for n in names)
fall=[i for i in range(1,len(data)) if data[i-1]&128 and not data[i]&128]
rise=[i for i in range(1,len(data)) if not data[i-1]&128 and data[i]&128]
assert len(fall)==16,(len(fall),fall)
rows=[]
for n,f in enumerate(fall):
    r=next((x for x in rise if x>f),None)
    assert r is not None,(n,'missing rise')
    assert 98<=r-f<=102,(n,'low duration',r-f)
    assert all((v&15)==n for v in data[f:r]),(n,'data during assertion')
    # Ignore initial lead-in if capture began at trigger; later data transitions
    # must establish setup/hold, measured independently of firmware source.
    before=f
    while before>0 and data[before-1]&15==n: before-=1
    after=r
    while after<len(data) and data[after]&15==n: after+=1
    if n>0: assert 98<=f-before<=102,(n,'setup',f-before)
    if n<15: assert 98<=after-r<=102,(n,'hold',after-r)
    if n>0: assert 298<=f-fall[n-1]<=302,(n,'period',f-fall[n-1])
    rows.append(dict(value=n,fall_sample=f,rise_sample=r,low_us=r-f,setup_us=f-before,hold_us=after-r))
print(json.dumps(dict(capture=str(p),sample_rate=1000000,assertions=16,values=list(range(16)),measurements=rows),indent=2))
