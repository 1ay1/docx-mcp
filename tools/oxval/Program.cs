// oxval: test-only conformance oracle. Runs Microsoft's Open XML SDK
// validator (schema + semantic + package rules) over .docx files and prints
// one line per error. Exit code is the number of files with errors.
using DocumentFormat.OpenXml;
using DocumentFormat.OpenXml.Packaging;
using DocumentFormat.OpenXml.Validation;

int bad = 0;
var version = FileFormatVersions.Microsoft365;
var files = new List<string>();
foreach (var a in args)
{
    if (a == "--2007") version = FileFormatVersions.Office2007;
    else if (a == "--2010") version = FileFormatVersions.Office2010;
    else if (a == "--2019") version = FileFormatVersions.Office2019;
    else files.Add(a);
}
foreach (var path in files)
{
    try
    {
        using var doc = WordprocessingDocument.Open(path, false);
        var v = new OpenXmlValidator(version);
        var errs = v.Validate(doc).ToList();
        if (errs.Count > 0) bad++;
        Console.WriteLine($"{path}: {errs.Count} error(s)");
        foreach (var e in errs.Take(50))
            Console.WriteLine($"  [{e.ErrorType}] {e.Part?.Uri} {e.Path?.XPath} :: {e.Description}");
    }
    catch (Exception ex)
    {
        bad++;
        Console.WriteLine($"{path}: OPEN FAILED: {ex.GetType().Name}: {ex.Message}");
    }
}
return bad;
