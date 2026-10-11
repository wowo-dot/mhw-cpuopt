"""Builds the private IDA database from the clean game image (run by idat).

  idat -A -Opdb:off -S"tools/ida_build.py" -L<log> <copy of the clean image>

Waits for auto-analysis, saves the database next to the input and exits.
-Opdb:off keeps IDA from asking Microsoft's symbol server for World300.pdb.
Names and comments come afterwards from CMFM's Ghidra export of Fexty's
database (docs/research/ida-database.md).
"""
import ida_auto
import ida_loader
import idc

ida_auto.auto_wait()
ida_loader.save_database(idc.get_idb_path(), 0)
idc.qexit(0)
