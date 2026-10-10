# Virtual Display Driver

GameWindowMapper embeds the original AMD64 driver from VirtualDrivers release **25.7.23**, under the MIT license in `LICENSE.txt`.

- Source and release: https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/tag/25.7.23
- Original license: https://github.com/VirtualDrivers/Virtual-Display-Driver/blob/25.7.23/LICENSE
- Asset: `VirtualDisplayDriver-x86.Driver.Only.zip`. Despite the asset name, this INF declares `NTamd64` and its DLL is AMD64.
- ZIP SHA256: `e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a`
- MttVDD.inf SHA256: `550d211fe481e74dfe3f9d724ed78be48b3a9113405965d683d9373e8d672f5d`
- MttVDD.dll SHA256: `c9ca837f57a98fbd43bc416a7f535a95843626e7759eaf85cf0cd7ce334dbb05`
- mttvdd.cat SHA256: `08a0093fc9b2e32b287a6f8a77ca4de0a31830d29fc33d2b13a918dc859468f6`

The INF, DLL and catalog are unchanged. GameWindowMapper builds its own installer and recovery helper; it does not include VDC, DevCon, or a modified/re-signed driver. The license is embedded as RCDATA and extracted next to the runtime driver package. Building downloads only the pinned release into the ignored `out/vendor/virtual-display-driver/25.7.23` cache; no driver binaries are added to Git.
