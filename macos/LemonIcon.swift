import AppKit

/// Shared vector geometry; raster sizes are generated only for macOS packaging.
enum LemonIcon {
    static func image(size:CGFloat,template:Bool) -> NSImage {
        let image=NSImage(size:NSSize(width:size,height:size),flipped:false) { bounds in
            let transform=NSAffineTransform(); transform.scale(by:size/100); transform.concat()
            if !template {
                NSColor(calibratedRed:0.13,green:0.25,blue:0.19,alpha:1).setFill()
                NSBezierPath(roundedRect:NSRect(x:2,y:2,width:96,height:96),xRadius:22,yRadius:22).fill()
            }
            let lemon=NSBezierPath()
            lemon.move(to:NSPoint(x:15,y:42))
            lemon.curve(to:NSPoint(x:77,y:76),controlPoint1:NSPoint(x:20,y:75),controlPoint2:NSPoint(x:48,y:90))
            lemon.line(to:NSPoint(x:86,y:79)); lemon.line(to:NSPoint(x:83,y:68))
            lemon.curve(to:NSPoint(x:23,y:25),controlPoint1:NSPoint(x:89,y:34),controlPoint2:NSPoint(x:51,y:9))
            lemon.line(to:NSPoint(x:13,y:22)); lemon.line(to:NSPoint(x:15,y:42)); lemon.close()
            (template ? NSColor.black : NSColor(calibratedRed:0.99,green:0.83,blue:0.23,alpha:1)).setFill(); lemon.fill()
            let wave=NSBezierPath(); wave.lineWidth=template ? 6 : 5; wave.lineCapStyle = .round; wave.lineJoinStyle = .round
            wave.move(to:NSPoint(x:30,y:48)); wave.line(to:NSPoint(x:38,y:48)); wave.line(to:NSPoint(x:43,y:60)); wave.line(to:NSPoint(x:50,y:35)); wave.line(to:NSPoint(x:57,y:65)); wave.line(to:NSPoint(x:63,y:48)); wave.line(to:NSPoint(x:72,y:48))
            if template {
                NSGraphicsContext.current?.compositingOperation = .destinationOut
                NSColor.black.setStroke(); wave.stroke()
                NSGraphicsContext.current?.compositingOperation = .sourceOver
            } else { NSColor(calibratedRed:0.13,green:0.25,blue:0.19,alpha:1).setStroke(); wave.stroke() }
            return true
        }
        image.isTemplate=template; image.accessibilityDescription="LemonVox"; return image
    }
}
