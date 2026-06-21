# C Code Static Analyzer & Optimizer

A production-grade, single-pass static analysis tool built using the **LLVM/Clang C++ API**. This project parses raw C code into a Control Flow Graph (CFG) and performs advanced dataflow analysis, security auditing, and compiler-level optimizations, all visualized through an interactive **Python/Streamlit** dashboard.

## Key Features
* **LLVM AST Parsing:** Leverages `RecursiveASTVisitor` to parse C source code directly without relying on custom regex or brittle parsers.
* **Control Flow Graph (CFG) Generation:** Automatically maps basic blocks, conditional branches, and function logic.
* **Dataflow Analysis (Fixed-Point Iteration):** Accurately tracks Live IN and Live OUT sets across the entire CFG until equations converge.
* **Advanced Optimizations:** 
  * Local Constant Folding & Propagation.
  * Statement-level Dead Code Elimination (DCE).  
  * Unreachable Code/Function Removal.
* **Natural Loop Detection:** Uses Dominator Tree math to strictly identify loop back-edges and loop headers.
* **Forward Security Taint Analysis:** Tracks user input (e.g., `scanf`) to identify dangerous sinks like Buffer Overflows, Format String Vulnerabilities, and Command Injections.

## Benchmarks & Visualizations

### 1. The Optimizer
*(Drag and drop your Benchmark 1 image here)*

Demonstrates Constant Folding reducing multiple math operations into a single variable, followed by aggressive Dead Code Elimination and Unreachable Code removal.

### 2. The Security Auditor
*(Drag and drop your Benchmark 2 image here)*

Highlights the Forward Taint Analysis successfully flagging `printf` (Format String), `system` (Command Injection), and array indexing (Buffer Overflow) risks in red.

### 3. The Architecture Mapper
*(Drag and drop your Benchmark 3 image here)*

Showcases Natural Loop Detection via Dominator Trees, complete with visually rendered loop back-edges and continuous Live Variable sets.

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