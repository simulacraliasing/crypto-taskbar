// Rebuild the small native app icon. No npm dependencies.
import { writeFileSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const target = fileURLToPath(new URL('../resources/', import.meta.url));
mkdirSync(target, { recursive: true });
const sizes = [16, 32, 48, 64];
const images = sizes.map(size => {
  const maskStride = Math.ceil(size / 32) * 4;
  const out = Buffer.alloc(40 + size * size * 4 + maskStride * size);
  out.writeUInt32LE(40, 0); out.writeInt32LE(size, 4); out.writeInt32LE(size * 2, 8);
  out.writeUInt16LE(1, 12); out.writeUInt16LE(32, 14); out.writeUInt32LE(size * size * 4, 20);
  const points = [[8, 22], [13, 16], [18, 19], [24, 10]];
  const distance = (x, y, a, b) => {
    const dx = b[0] - a[0], dy = b[1] - a[1];
    const t = Math.max(0, Math.min(1, ((x-a[0])*dx+(y-a[1])*dy)/(dx*dx+dy*dy)));
    return Math.hypot(x-a[0]-t*dx, y-a[1]-t*dy);
  };
  for(let y=0;y<size;y++) for(let x=0;x<size;x++) {
    const sum=[0,0,0,0];
    for(let sy=0;sy<4;sy++) for(let sx=0;sx<4;sx++) {
      const px=(x+(sx+.5)/4)*32/size, py=(y+(sy+.5)/4)*32/size;
      if(Math.hypot(px-16,py-16)>15) continue;
      const line=points.slice(1).some((p,i)=>distance(px,py,points[i],p)<1.4);
      const color=line?[255,255,255]:[54,120,238];
      sum[0]+=color[0];sum[1]+=color[1];sum[2]+=color[2];sum[3]++;
    }
    const i=40+((size-1-y)*size+x)*4, n=sum[3];
    if(n){out[i]=Math.round(sum[2]/n);out[i+1]=Math.round(sum[1]/n);out[i+2]=Math.round(sum[0]/n);out[i+3]=Math.round(n*255/16);}
  }
  return out;
});
const header=Buffer.alloc(6+sizes.length*16);header.writeUInt16LE(1,2);header.writeUInt16LE(sizes.length,4);
let offset=header.length;
images.forEach((data,i)=>{const p=6+i*16;header[p]=sizes[i];header[p+1]=sizes[i];header.writeUInt16LE(1,p+4);header.writeUInt16LE(32,p+6);header.writeUInt32LE(data.length,p+8);header.writeUInt32LE(offset,p+12);offset+=data.length;});
writeFileSync(target+'app.ico',Buffer.concat([header,...images]));
