PES-VCS — Version Control System Lab Report
Name: Kavya Pandappa Korti
SRN: PES1UG24CS575  
Repository: PES1UG24CS575-pes-vcs  
Platform: Ubuntu 22.04
---
Table of Contents
Phase 1 — Object Storage Foundation
Phase 2 — Tree Objects
Phase 3 — The Index (Staging Area)
Phase 4 — Commits and History
Phase 5 — Branching and Checkout (Analysis)
Phase 6 — Garbage Collection (Analysis)
---
Phase 1 — Object Storage Foundation
Concepts: Content-addressable storage, directory sharding, atomic writes, SHA-256 hashing  
Files implemented: `object.c` — `object_write`, `object_read`
What Was Implemented
`object_write` prepends a type header (`blob <size>\0`, `tree <size>\0`, or `commit <size>\0`) to the data, computes a SHA-256 hash of the full object, then writes atomically using a temp-file-then-rename pattern. Objects are sharded into subdirectories named by the first two hex characters of the hash.
`object_read` reads the file, parses the header to extract the object type and size, recomputes the hash and compares it to the filename for integrity verification, then returns the data portion after the null byte.
Screenshot 1A — Phase 1 Tests Passing
> <img width="763" height="219" alt="image" src="https://github.com/user-attachments/assets/a364ec17-3b52-4fc9-8fb8-a8174ba079c1" />

Screenshot 1B — Sharded Object Directory
<img width="762" height="470" alt="image" src="https://github.com/user-attachments/assets/1478776c-522a-4c83-b36c-4893d5cb8e40" />

---
Phase 2 — Tree Objects
Concepts: Directory representation, recursive structures, file modes and permissions  
Files implemented: `tree.c` — `tree_from_index`
What Was Implemented
`tree_from_index` builds a tree hierarchy from the staged index. It handles nested paths (e.g., `src/main.c` creates a `src` subtree), recursively writes all tree objects to the object store using `object_write`, and returns the root tree hash. Entries are sorted deterministically to ensure the same content always produces the same hash.
Screenshot 2A — Phase 2 Tests Passing
<img width="765" height="158" alt="image" src="https://github.com/user-attachments/assets/fa2047f5-1116-458a-91c3-a3da9d074a88" />

Screenshot 2B — Raw Tree Object (xxd)
<img width="754" height="424" alt="image" src="https://github.com/user-attachments/assets/cee4873a-c5b0-4a70-8494-72ec38bfb095" />

Commands to generate this screenshot:
```bash
# Find which objects are trees
for f in $(find .pes/objects -type f); do
  result=$(strings "$f" | head -1)
  echo "$f: $result"
done

# Once you find one starting with "tree", run:
xxd .pes/objects/XX/YYYY... | head -20
```
---
Phase 3 — The Index (Staging Area)
Concepts: File format design, atomic writes, change detection using metadata  
Files implemented: `index.c` — `index_load`, `index_save`, `index_add`
What Was Implemented
`index_load` reads `.pes/index`, parsing each line in the format `<mode> <hash-hex> <mtime> <size> <path>`. If the file doesn't exist, it initialises an empty index (not an error).
`index_save` sorts entries by path, writes them to a temp file, calls `fsync()`, then atomically renames the temp file to `.pes/index`.
`index_add` reads the target file, writes its contents as a blob object via `object_write`, and updates (or inserts) the index entry using the blob's hash and the file's current metadata.
Screenshot 3A — init → add → status
<img width="755" height="183" alt="image" src="https://github.com/user-attachments/assets/ceba1735-19f7-446d-a4cc-a708b42ea5bf" />

Screenshot 3B — Index File Contents
<img width="765" height="149" alt="image" src="https://github.com/user-attachments/assets/d832c6e9-b9af-4e7d-978d-ec73ad6f6a98" />

---
Phase 4 — Commits and History
Concepts: Linked structures on disk, reference files, atomic pointer updates  
Files implemented: `commit.c` — `commit_create`
What Was Implemented
`commit_create` builds a tree from the current index via `tree_from_index()`, reads the current HEAD to obtain the parent commit hash (absent for the first commit), retrieves the author string from `pes_author()`, serialises the commit object, writes it to the object store, and atomically updates the branch ref pointed to by HEAD.
Screenshot 4A — pes log (Three Commits)


Screenshot 4B — Object Store Growth
<img width="761" height="419" alt="image" src="https://github.com/user-attachments/assets/effda357-9461-4719-abaa-024b754d0ac5" />
<img width="757" height="43" alt="image" src="https://github.com/user-attachments/assets/03162b56-f27c-4cdb-8ce9-5f5e8f7a79a1" />

Screenshot 4C — Reference Chain
<img width="756" height="117" alt="image" src="https://github.com/user-attachments/assets/9b7e27fd-05f6-47a8-9223-b4d21055396b" />

Screenshot — Full Integration Test
<img width="759" height="585" alt="image" src="https://github.com/user-attachments/assets/1a42fd66-59c7-4222-9c1a-3a1e98b7d411" />
<img width="766" height="554" alt="image" src="https://github.com/user-attachments/assets/002babe0-55f3-46e6-8c8b-befe6535d662" />
<img width="765" height="615" alt="image" src="https://github.com/user-attachments/assets/74f19dd2-9e7b-473f-9aa9-3d2ccd28e1e1" />
<img width="762" height="146" alt="image" src="https://github.com/user-attachments/assets/3acf4899-b06e-4952-8ac4-d9174417d443" />

---
Phase 5 — Branching and Checkout (Analysis)
Q5.1 — Implementing `pes checkout <branch>`
To implement `pes checkout <branch>`, the following steps and file changes are required:
Files that need to change in `.pes/`:
`.pes/HEAD` must be updated to `ref: refs/heads/<branch>` (or to the commit hash directly if checking out a detached state).
The working directory files must be updated to match the tree snapshot pointed to by the target branch's latest commit.
Algorithm:
Read `.pes/refs/heads/<branch>` to get the target commit hash.
Read the commit object to get its root tree hash.
Recursively walk the tree object, and for each blob entry, read the blob from the object store and write it to the corresponding path in the working directory.
Delete any working directory files that exist in the current HEAD tree but not in the target tree.
Rewrite `.pes/HEAD` to point to the new branch.
Rebuild the index to match the checked-out tree.
What makes this complex:
Subdirectory creation: nested trees require creating intermediate directories before writing files.
Deletions: files present in the old tree but absent from the new tree must be removed, which requires diffing two tree structures.
Index synchronisation: the index must exactly reflect the checked-out tree so that `pes status` reports a clean state.
Conflict detection: if there are uncommitted local changes, the checkout must be refused (see Q5.2).
---
Q5.2 — Detecting Dirty Working Directory Conflicts
Without running a full diff tool, the conflict can be detected using only the index and the object store:
For each file in the current index, re-read the working directory file, hash its contents with `object_write` (or just compute the SHA-256), and compare the resulting hash against the hash stored in the index entry.
If they differ, the file has been modified and is unstaged — it is "dirty."
For each dirty file, look up whether the same path exists in the target branch's tree (by walking the target tree object). If the target branch has a different blob hash for that path than the current branch's tree, then the checkout would overwrite the user's unsaved change — this is a conflict, and the checkout must be refused with an error.
Files that are dirty but identical between source and target branches can be safely ignored (the working directory version is already what checkout would produce).
This approach uses only: the index (for tracking the last-staged hash), the object store (for reading tree objects), and `stat()`/file reads on the working directory. No separate diff algorithm is needed.
---
Q5.3 — Detached HEAD and Recovery
What happens in detached HEAD:  
When `HEAD` contains a raw commit hash (e.g., `a1b2c3...`) instead of `ref: refs/heads/main`, new commits are created and chained correctly — each commit still records its parent. However, no branch file is updated, so there is no named pointer to the new commits. They exist in the object store but are not reachable from any branch.
Recovery options:
If the user knows the commit hash (visible in the terminal output from `pes commit`), they can create a new branch pointing to it:
```bash
  echo "<commit-hash>" > .pes/refs/heads/recovery-branch
  ```
Then update HEAD to point to that branch.
If the hash was lost, the commits are still in `.pes/objects/` and can be found by scanning all commit objects and reconstructing the chain (similar to `git reflog`).
Once a branch ref points to the detached commit chain, the commits become fully reachable and protected from garbage collection.
---
Phase 6 — Garbage Collection (Analysis)
Q6.1 — Finding and Deleting Unreachable Objects
Algorithm (mark-and-sweep):
Mark phase — collect all reachable objects:
Start from every branch ref in `.pes/refs/heads/` and from HEAD.
For each reachable commit hash, add it to a `reachable` set (a hash set / `unordered_set<string>`).
Parse the commit to get its tree hash and parent hash(es); add those to a work queue.
For each tree hash in the queue, parse the tree, add the tree hash to `reachable`, and enqueue all blob hashes and subtree hashes it references.
Repeat until the queue is empty. Every hash in `reachable` is live.
Sweep phase — delete unreachable objects:
Walk every file under `.pes/objects/` using `find` or `opendir/readdir`.
Reconstruct the full hash from the two-character directory prefix and the filename.
If the hash is not in `reachable`, delete the file.
Data structure: A hash set (`unordered_set<string>` in C++ or a hash table in C) gives O(1) average lookup and insertion, which is ideal for large repositories.
Estimate for 100,000 commits / 50 branches:  
Assuming an average of 5 objects per commit (1 commit + 1–2 trees + 2–3 blobs), the total number of reachable objects is roughly 500,000. The mark phase visits all 500,000 reachable objects. The sweep phase scans all objects on disk (potentially more if many are unreachable). Total: on the order of 500,000–1,000,000 object visits.
---
Q6.2 — GC Race Condition with Concurrent Commits
The race condition:
GC starts its mark phase. It reads all branch refs and builds the `reachable` set. At this moment, a new commit is being prepared concurrently.
The concurrent commit has already written new blob and tree objects to the object store but has not yet updated the branch ref.
GC's mark phase completes. The new blobs and trees are not reachable from any ref yet, so they are not added to the `reachable` set.
GC's sweep phase deletes those brand-new objects.
The concurrent commit now tries to write the commit object referencing those deleted blobs/trees and then updates the branch ref — but the objects it points to are gone. The repository is now corrupt.
How Git avoids this:  
Git addresses this through a combination of strategies:
Grace period: Git's GC (`git gc`) will not delete any object created less than 2 weeks ago by default (`gc.pruneExpire`). Since a commit operation completes in milliseconds, any recently written object is safe.
Loose object timestamp: The mtime of loose object files is checked; objects newer than the grace period are never pruned regardless of reachability.
Lock files: Git uses lock files on ref updates so that GC and commit operations can detect contention and retry.
The core principle is: never delete an object that was written after the GC's mark phase began, because that object might be part of an in-progress commit that has not yet updated any ref.
---
Submission Checklist
Phase	ID	Screenshot	Status
1	1A	`./test_objects` all tests passing	✅

1	1B	`find .pes/objects -type f` sharded structure	✅
2	2A	`./test_tree` all tests passing	✅
2	2B	`xxd` of raw tree object (first 20 lines)	⬜ (pending — see Phase 2 instructions above)
3	3A	`pes init → pes add → pes status`	✅
3	3B	`cat .pes/index`	✅
4	4A	`pes log` with three commits	✅
4	4B	`find .pes -type f | sort` object growth	✅
4	4C	`cat .pes/refs/heads/main` + `cat .pes/HEAD`	✅
Final	—	`make test-integration` all passed	
---
