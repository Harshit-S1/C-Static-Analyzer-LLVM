# LLVM-Based C Static Analyzer & Optimizer

![C++](https://img.shields.io/badge/C++-17-blue)
![LLVM](https://img.shields.io/badge/LLVM-Clang-orange)
![Streamlit](https://img.shields.io/badge/Streamlit-Dashboard-red)

A static analysis and optimization tool for C programs built with **C++** using **LLVM/Clang LibTooling**. The project parses C source code into a Control Flow Graph (CFG) and performs dataflow analysis, security auditing, and compiler optimizations, all visualized through an interactive **Python/Streamlit** dashboard.

## Tech Stack

- **Language:** C++17, Python
- **Compiler Framework:** LLVM/Clang LibTooling
- **AST Traversal:** RecursiveASTVisitor
- **Graph Visualization:** Graphviz
- **Web Interface:** Streamlit
- **Build System:** Makefile
  
## Key Features
* **LLVM AST Parsing:** Uses LLVM/Clang's `RecursiveASTVisitor` to traverse the AST and analyze C source code without custom parsers.
* **Control Flow Graph (CFG) Generation:** Constructs Control Flow Graphs by identifying basic blocks, conditional branches, and control-flow edges.
* **Dataflow Analysis (Fixed-Point Iteration):** Computes Live IN and Live OUT sets using iterative fixed-point dataflow analysis until convergence.
* **Advanced Optimizations:** 
  * Local Constant Folding & Propagation.
  * Statement-level Dead Code Elimination (DCE).  
  * Unreachable Code/Function Removal.
* **Natural Loop Detection:** Uses Dominator Trees to identify natural loops, back-edges, and loop headers.
* **Forward Security Taint Analysis:** Tracks tainted user input (e.g., `scanf`) to detect dangerous sinks such as format string vulnerabilities, command injection, and potential buffer overflow risks.

## Visualizations

### 1. The Optimizer
<img width="1475" height="642" alt="Screenshot 2026-06-22 003608" src="https://github.com/user-attachments/assets/633b5281-0a68-4429-b7b9-bee6b205e28f" />

Demonstrates Constant Folding and Constant Propagation simplifying arithmetic expressions, followed by Dead Code Elimination and Unreachable Code Removal.

### 2. The Security Auditor
<img width="1483" height="810" alt="Screenshot 2026-06-22 004015" src="https://github.com/user-attachments/assets/db35edad-f445-40c6-aa89-18172e446685" />

Highlights the Forward Taint Analysis successfully flagging `printf` (Format String), `system` (Command Injection), and array indexing (Buffer Overflow) risks in red.

### 3. The Architecture Mapper
<img width="1456" height="861" alt="Screenshot 2026-06-22 004147" src="https://github.com/user-attachments/assets/cfbffd42-7b42-45e8-99af-8b8ec2704e57" />

Showcases Natural Loop Detection using Dominator Trees, along with visualized loop back-edges and Live Variable analysis results.

## How to Run

**Prerequisites:**
* LLVM/Clang (v10.0+)
* Python 3.8+
* Graphviz (`sudo apt install graphviz` or Windows equivalent)

**Build the C++ Analyzer Backend:**
```bash
make all
```

**Run the Streamlit Dashboard:**
```bash
pip install streamlit streamlit-code-editor
streamlit run webinterface.py
```
