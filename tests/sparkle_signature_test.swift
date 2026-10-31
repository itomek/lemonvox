import Foundation
import CryptoKit
@main struct SignatureCheck {
 static func main() throws {
  let a=CommandLine.arguments
  guard a.count==4,let publicBytes=Data(base64Encoded:a[2]),let signature=Data(base64Encoded:a[3]) else { exit(2) }
  let key=try Curve25519.Signing.PublicKey(rawRepresentation:publicBytes)
  let data=try Data(contentsOf:URL(fileURLWithPath:a[1]))
  guard key.isValidSignature(signature,for:data) else { print("Rejected invalid update signature");exit(1) }
  print("PASS: independent public-key verification of final archive bytes")
 }
}
