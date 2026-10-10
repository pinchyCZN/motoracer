// ghidra_backup_gzf.java
//
// Write a .gzf snapshot of the current program into
// decomp/ghidra-backups/, named MOTO.EXE-<yyyy-MM-dd>-<HHmm>.gzf to
// match the ones already there. It takes no arguments - the time is
// the name.
//
//   tools/ghidra_backup_gzf.java
//
// THE NAME IS THE CLOCK, NOT A TOPIC. This took a topic argument
// once; every backup in that folder is a timestamp and the README
// documents timestamps, so the argument was the odd one out. Two
// runs inside one minute decline rather than overwrite, and nothing
// is lost by that - the file already there IS that minute.
//
// USE packFile, NOT GzfExporter. Both produce the same format - the
// files are byte-compatible with the existing backups, magic aced0005 -
// but GzfExporter fails when the script is driven through the MCP
// server:
//
//     GzfExporter log: Unexpected exception exporting file:
//                      Unable to lock due to active transaction
//
// because the MCP call already holds a transaction open and the
// exporter wants an exclusive lock. DomainFile.packFile does not, so it
// works from either the GUI or the MCP. That is the whole reason this
// script exists rather than a line in a note.
//
// IT SAVES FIRST. An unsaved program would otherwise snapshot the
// last-written state and silently lose the session's work, which is
// exactly the failure a backup is supposed to prevent. The script
// reports isChanged() before and after so the log says whether there
// was anything to flush.
//
// @category MotoRacer

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.Date;

import ghidra.app.script.GhidraScript;
import ghidra.framework.model.DomainFile;

public class ghidra_backup_gzf extends GhidraScript {

	private static final String BACKUP_DIR =
		"N:\\DEV\\MOTORACER\\decomp\\ghidra-backups";

	@Override
	public void run() throws Exception {
		File dir = new File(BACKUP_DIR);
		if (!dir.isDirectory()) {
			println("backup: " + dir + " does not exist - refusing to "
			        + "create it, check the path");
			return;
		}

		DomainFile df = currentProgram.getDomainFile();
		println("backup: " + df.getPathname()
		        + "  changed=" + df.isChanged());

		if (df.isChanged()) {
			df.save(monitor);
			println("backup: saved, changed=" + df.isChanged());
		}

		String stamp =
		    new SimpleDateFormat("yyyy-MM-dd-HHmm").format(new Date());
		String name  = currentProgram.getName() + "-" + stamp
		             + ".gzf";
		File out = new File(dir, name);

		if (out.exists()) {
			// A second run inside the same MINUTE. Declining loses
			// nothing: the file already there is this same session,
			// to the minute.
			println("backup: " + name + " already exists - a "
			        + "snapshot was taken this minute");
			return;
		}

		df.packFile(out, monitor);
		println("backup: wrote " + out.getAbsolutePath()
		        + "  " + out.length() + " bytes");
	}
}
