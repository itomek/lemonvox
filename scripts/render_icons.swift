import AppKit
let root=URL(fileURLWithPath:CommandLine.arguments[1]); try FileManager.default.createDirectory(at:root,withIntermediateDirectories:true)
for (name,size) in [("icon_16x16",16),("icon_16x16@2x",32),("icon_32x32",32),("icon_32x32@2x",64),("icon_128x128",128),("icon_128x128@2x",256),("icon_256x256",256),("icon_256x256@2x",512),("icon_512x512",512),("icon_512x512@2x",1024)] {
 let image=LemonIcon.image(size:CGFloat(size),template:false)
 let rep=NSBitmapImageRep(data:image.tiffRepresentation!)!
 try rep.representation(using:.png,properties:[:])!.write(to:root.appendingPathComponent(name+".png"))
}
let preview=NSImage(size:NSSize(width:600,height:200)); preview.lockFocus()
for (x,dark) in [(0,false),(300,true)] {
 (dark ? NSColor.black : NSColor.white).setFill(); NSRect(x:x,y:0,width:300,height:200).fill()
 LemonIcon.image(size:128,template:false).draw(in:NSRect(x:x+20,y:36,width:128,height:128))
 let glyph=LemonIcon.image(size:20,template:true)
 let tinted=NSImage(size:glyph.size); tinted.lockFocus(); glyph.draw(at:.zero,from:.zero,operation:.sourceOver,fraction:1)
 (dark ? NSColor.white : NSColor.black).setFill(); NSRect(origin:.zero,size:glyph.size).fill(using:.sourceIn); tinted.unlockFocus()
 tinted.draw(in:NSRect(x:x+200,y:90,width:20,height:20))
}
preview.unlockFocus(); let rep=NSBitmapImageRep(data:preview.tiffRepresentation!)!
try rep.representation(using:.png,properties:[:])!.write(to:root.deletingLastPathComponent().appendingPathComponent("icon-preview.png"))
