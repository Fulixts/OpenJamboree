// nsp_extract: unpacks YOUR OWN Switch game dump (base .nsp + update .nsp) into exefs/ + romfs/ with YOUR OWN keys.
// usage: nsp_extract <prod.keys> <base.nsp> <update.nsp> <out dir>
// Uses LibHac (BSD 3-clause). Keys and game files never leave the PC; nothing is downloaded.
using LibHac.Common; using LibHac.Common.Keys; using LibHac.Fs; using LibHac.Fs.Fsa; using LibHac.FsSystem;
using LibHac.Tools.Es; using LibHac.Tools.Fs; using LibHac.Tools.FsSystem; using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Spl; using LibHac.Ncm;

static void Say(string m) { Console.WriteLine(m); Console.Out.Flush(); }

if (args.Length < 4) { Console.Error.WriteLine("usage: nsp_extract <prod.keys> <base.nsp> <update.nsp> <out dir>"); return 2; }
var ks = ExternalKeyReader.ReadKeyFile(args[0]);

(Nca nca, ulong id) OpenProgram(string nsp, string what) {
    var pfs = new PartitionFileSystem();
    pfs.Initialize(new LocalStorage(nsp, FileAccess.Read)).ThrowIfFailure();
    // tickets first, so rights-id NCAs can be decrypted
    foreach (var e in pfs.EnumerateEntries("*.tik", SearchOptions.Default)) {
        using var f = new UniqueRef<IFile>();
        pfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        var tik = new Ticket(new BinaryReader(f.Get.AsStream()));
        ks.ExternalKeySet.Add(new RightsId(tik.RightsId), new AccessKey(tik.GetTitleKey(ks)));
    }
    foreach (var e in pfs.EnumerateEntries("*.nca", SearchOptions.Default)) {
        if (e.Name.EndsWith(".cnmt.nca")) continue;
        var f = new UniqueRef<IFile>();
        pfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
        var nca = new Nca(ks, f.Release().AsStorage());
        if (nca.Header.ContentType == NcaContentType.Program) {
            Say($"{what}: program {nca.Header.TitleId:x16} ({e.Name})");
            return (nca, nca.Header.TitleId);
        }
    }
    throw new Exception($"{what}: no program NCA in {nsp} (is it the right file?)");
}

var (baseNca, baseId) = OpenProgram(args[1], "base");
var (patchNca, patchId) = OpenProgram(args[2], "update");
if (baseId != patchId) throw new Exception($"the update ({patchId:x16}) is not for this game ({baseId:x16})");

var outDir = System.IO.Path.GetFullPath(args[3]);
Directory.CreateDirectory(System.IO.Path.Combine(outDir, "exefs"));
Directory.CreateDirectory(System.IO.Path.Combine(outDir, "romfs"));
Say("progress 0.05 Extracting the game code (exefs)");
var code = patchNca.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid);
code.CopyDirectory(new LocalFileSystem(System.IO.Path.Combine(outDir, "exefs")), "/", "/").ThrowIfFailure();
Say("progress 0.10 Extracting the game data (romfs, base + update, about 5 GB)");
var data = baseNca.OpenFileSystemWithPatch(patchNca, NcaSectionType.Data, IntegrityCheckLevel.None);
data.CopyDirectory(new LocalFileSystem(System.IO.Path.Combine(outDir, "romfs")), "/", "/").ThrowIfFailure();
Say("done");
return 0;

