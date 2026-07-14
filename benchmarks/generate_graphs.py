import os
import re
import sys
import json
import shutil
import argparse
import tempfile
import subprocess
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Patch

# Anchor directory to repository root
script_dir = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.abspath(os.path.join(script_dir, ".."))
os.chdir(repo_root)

def run_zig():
    print("=== Running Zig benchmarks ===")
    res = subprocess.run(["zig", "build", "run", "-Doptimize=ReleaseFast", "--", "--quick"],
                         capture_output=True, text=True, check=True)
    out = res.stdout + "\n" + res.stderr
    
    throughput = float(re.search(r"Throughput \(ops/s\):\s+([\d\.]+)", out).group(1))
    hit_ratio = float(re.search(r"Hit Ratio:\s+([\d\.]+)%", out).group(1))
    p50 = float(re.search(r"Latency \(p50\):\s+([\d\.]+) us", out).group(1))
    p99 = float(re.search(r"Latency \(p99\):\s+([\d\.]+) us", out).group(1))
    p999 = float(re.search(r"Latency \(p99\.9\):\s+([\d\.]+) us", out).group(1))
    p9999 = float(re.search(r"Latency \(p99\.99\):\s+([\d\.]+) us", out).group(1))
    
    return {
        "throughput": throughput,
        "hit_ratio": hit_ratio,
        "p50": p50,
        "p99": p99,
        "p999": p999,
        "p9999": p9999
    }

def run_go():
    print("=== Running Go benchmarks ===")
    archive_path = os.path.abspath("zig-out/lib/libcaeneus.a")
    env = os.environ.copy()
    env["CGO_ENABLED"] = "1"
    env["CGO_LDFLAGS"] = archive_path
    
    res = subprocess.run([
        "go", "-C", "ext/go/benchmarks", "test",
        "-bench=BenchmarkProfile[AB]_",
        "-benchmem",
        "-benchtime=500ms",
        "-count=1"
    ], capture_output=True, text=True, env=env)
    
    results = []
    pattern = r"BenchmarkProfile(A|B)_([A-Za-z0-9_]+?)(?:-\d+)?\s+\d+\s+([\d\.]+)\s+ns/op"
    for line in res.stdout.splitlines():
        match = re.search(pattern, line)
        if match:
            profile = match.group(1)
            impl = match.group(2)
            ns_op = float(match.group(3))
            
            throughput = 1e9 / ns_op
            latency_us = ns_op / 1000.0
            
            results.append({
                "profile": profile,
                "implementation": impl,
                "throughput": throughput,
                "latency_us": latency_us
            })
    return results

def run_python():
    print("=== Running Python benchmarks ===")
    res = subprocess.run([
        "uv", "run", "--project", "ext/python", "python", "ext/python/benchmark.py",
        "--profile", "all",
        "--cache", "all",
        "--operations", "10000",
        "--keys", "2048",
        "--sample-rate", "100"
    ], capture_output=True, text=True, check=True)
    
    results = []
    for line in res.stdout.splitlines():
        if line.strip().startswith("{"):
            data = json.loads(line)
            profile = "A" if "A_read_heavy" in data["profile"] else "B"
            results.append({
                "profile": profile,
                "implementation": data["implementation"],
                "throughput": data["operations_per_second"],
                "p50_us": data["p50_us"],
                "p99_us": data["p99_us"]
            })
    return results

def run_node():
    print("=== Running Node benchmarks ===")
    res = subprocess.run([
        "npm", "--prefix", "ext/node", "run", "benchmark", "--",
        "--profile", "all",
        "--cache", "all",
        "--operations", "10000",
        "--keys", "2048",
        "--sample-rate", "100"
    ], capture_output=True, text=True, check=True)
    
    results = []
    for line in res.stdout.splitlines():
        if line.strip().startswith("{"):
            data = json.loads(line)
            profile = "A" if "A_read_heavy" in data["profile"] else "B"
            results.append({
                "profile": profile,
                "implementation": data["implementation"],
                "throughput": data["operations_per_second"],
                "p50_us": data["p50_us"],
                "p99_us": data["p99_us"]
            })
    return results

def plot_zig(data, output_path):
    fig, ax = plt.subplots(figsize=(6, 5))
    fig.suptitle("Caeneus Zig Native Performance", fontsize=14, weight='bold')
    
    latencies = [data["p50"], data["p99"], data["p999"], data["p9999"]]
    labels = ["p50", "p99", "p99.9", "p99.99"]
    
    bars = ax.bar(labels, latencies, color='#E63946', alpha=0.9, width=0.5)
    ax.set_ylabel("Latency (microseconds)")
    ax.grid(axis='y', linestyle='--', alpha=0.5)
    
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    
    for bar in bars:
        height = bar.get_height()
        ax.annotate(f'{height:.3f} us',
                    xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3),
                    textcoords="offset points",
                    ha='center', va='bottom', fontsize=9, weight='bold')
                    
    info_text = f"Throughput: {data['throughput']:,.0f} ops/s\nHit Ratio: {data['hit_ratio']:.1f}%"
    ax.text(0.95, 0.95, info_text, transform=ax.transAxes, fontsize=10,
            verticalalignment='top', horizontalalignment='right',
            bbox=dict(boxstyle='round,pad=0.5', facecolor='white', alpha=0.8, edgecolor='#ccc'))
            
    plt.tight_layout()
    plt.savefig(output_path, dpi=150)
    plt.close()

def plot_comparison(lang, data, output_path):
    data_a = [d for d in data if d["profile"] == "A"]
    data_b = [d for d in data if d["profile"] == "B"]
    
    def sort_key(d):
        impl = d["implementation"].lower()
        if "caeneus" in impl:
            return (0, impl)
        return (1, impl)
        
    data_a = sorted(data_a, key=sort_key)
    data_b = sorted(data_b, key=sort_key)
    
    fig, axs = plt.subplots(2, 2, figsize=(12, 10))
    fig.suptitle(f"Caeneus vs Competitors - {lang.upper()}", fontsize=16, weight='bold', y=0.98)
    
    # Custom legend elements
    thru_legend = [
        Patch(facecolor='#E63946', edgecolor='none', label='Caeneus'),
        Patch(facecolor='#457B9D', edgecolor='none', label='Competitors'),
    ]
    
    lat_legend_go = thru_legend
    
    lat_legend_other = [
        Patch(facecolor='#E63946', edgecolor='none', label='p50 (Caeneus)'),
        Patch(facecolor='#9B2226', edgecolor='none', label='p99 (Caeneus)'),
        Patch(facecolor='#A8DADC', edgecolor='none', label='p50 (Competitors)'),
        Patch(facecolor='#457B9D', edgecolor='none', label='p99 (Competitors)'),
    ]
    
    # 1. Throughput Profile A (top-left)
    ax = axs[0, 0]
    impls_a = [d["implementation"] for d in data_a]
    throughputs_a = [d["throughput"] for d in data_a]
    colors_a = ['#E63946' if 'caeneus' in impl.lower() else '#457B9D' for impl in impls_a]
    bars = ax.bar(impls_a, throughputs_a, color=colors_a, alpha=0.9)
    ax.set_title("Throughput (Profile A: Read-Heavy)\n[Higher is Better]", fontsize=12, weight='bold')
    ax.set_ylabel("Operations / Second")
    ax.grid(axis='y', linestyle='--', alpha=0.5)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    ax.legend(handles=thru_legend, loc='upper right')
    for bar in bars:
        height = bar.get_height()
        ax.annotate(f'{height:,.0f}', xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=8)

    # 2. Throughput Profile B (top-right)
    ax = axs[0, 1]
    impls_b = [d["implementation"] for d in data_b]
    throughputs_b = [d["throughput"] for d in data_b]
    colors_b = ['#E63946' if 'caeneus' in impl.lower() else '#457B9D' for impl in impls_b]
    bars = ax.bar(impls_b, throughputs_b, color=colors_b, alpha=0.9)
    ax.set_title("Throughput (Profile B: Eviction Storm)\n[Higher is Better]", fontsize=12, weight='bold')
    ax.set_ylabel("Operations / Second")
    ax.grid(axis='y', linestyle='--', alpha=0.5)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    ax.legend(handles=thru_legend, loc='upper right')
    for bar in bars:
        height = bar.get_height()
        ax.annotate(f'{height:,.0f}', xy=(bar.get_x() + bar.get_width() / 2, height),
                    xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=8)

    # 3. Latency Profile A (bottom-left)
    ax = axs[1, 0]
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    if lang == "go":
        latencies_a = [d["latency_us"] for d in data_a]
        bars = ax.bar(impls_a, latencies_a, color=colors_a, alpha=0.9)
        ax.set_ylabel("Average Latency (microseconds)")
        ax.legend(handles=lat_legend_go, loc='upper right')
        for bar in bars:
            height = bar.get_height()
            ax.annotate(f'{height:.3f}', xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=8)
    else:
        x = np.arange(len(impls_a))
        width = 0.35
        p50s = [d["p50_us"] for d in data_a]
        p99s = [d["p99_us"] for d in data_a]
        p50_colors = ['#E63946' if 'caeneus' in impl.lower() else '#A8DADC' for impl in impls_a]
        p99_colors = ['#9B2226' if 'caeneus' in impl.lower() else '#457B9D' for impl in impls_a]
        bars50 = ax.bar(x - width/2, p50s, width, color=p50_colors, alpha=0.9)
        bars99 = ax.bar(x + width/2, p99s, width, color=p99_colors, alpha=0.9)
        ax.set_xticks(x)
        ax.set_xticklabels(impls_a)
        ax.set_ylabel("Latency (microseconds)")
        ax.legend(handles=lat_legend_other, loc='upper right')
        for bar in bars50:
            height = bar.get_height()
            ax.annotate(f'{height:.2f}', xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=8)
        for bar in bars99:
            height = bar.get_height()
            ax.annotate(f'{height:.2f}', xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=8)
                        
    ax.set_title("Latency (Profile A: Read-Heavy)\n[Lower is Better]", fontsize=12, weight='bold')
    ax.grid(axis='y', linestyle='--', alpha=0.5)

    # 4. Latency Profile B (bottom-right)
    ax = axs[1, 1]
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    if lang == "go":
        latencies_b = [d["latency_us"] for d in data_b]
        bars = ax.bar(impls_b, latencies_b, color=colors_b, alpha=0.9)
        ax.set_ylabel("Average Latency (microseconds)")
        ax.legend(handles=lat_legend_go, loc='upper right')
        for bar in bars:
            height = bar.get_height()
            ax.annotate(f'{height:.3f}', xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=8)
    else:
        x = np.arange(len(impls_b))
        width = 0.35
        p50s = [d["p50_us"] for d in data_b]
        p99s = [d["p99_us"] for d in data_b]
        p50_colors = ['#E63946' if 'caeneus' in impl.lower() else '#A8DADC' for impl in impls_b]
        p99_colors = ['#9B2226' if 'caeneus' in impl.lower() else '#457B9D' for impl in impls_b]
        bars50 = ax.bar(x - width/2, p50s, width, color=p50_colors, alpha=0.9)
        bars99 = ax.bar(x + width/2, p99s, width, color=p99_colors, alpha=0.9)
        ax.set_xticks(x)
        ax.set_xticklabels(impls_b)
        ax.set_ylabel("Latency (microseconds)")
        ax.legend(handles=lat_legend_other, loc='upper right')
        for bar in bars50:
            height = bar.get_height()
            ax.annotate(f'{height:.2f}', xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=8)
        for bar in bars99:
            height = bar.get_height()
            ax.annotate(f'{height:.2f}', xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3), textcoords="offset points", ha='center', va='bottom', fontsize=8)

    ax.set_title("Latency (Profile B: Eviction Storm)\n[Lower is Better]", fontsize=12, weight='bold')
    ax.grid(axis='y', linestyle='--', alpha=0.5)
    
    plt.tight_layout()
    plt.savefig(output_path, dpi=150)
    plt.close()

def build_project():
    print("Building native libraries...")
    subprocess.run(["zig", "build", "-Doptimize=ReleaseFast"], check=True)
    subprocess.run(["uv", "sync", "--project", "ext/python", "--extra", "benchmarks"], check=True)
    if not os.path.exists("ext/node/node_modules"):
        subprocess.run(["npm", "--prefix", "ext/node", "install"], check=True)

def push_to_benchmarks_branch(temp_img_dir):
    print("=== Uploading graphs to 'benchmarks' branch ===")
    repo_path = os.getcwd()
    
    with tempfile.TemporaryDirectory() as temp_git_dir:
        subprocess.run(["git", "clone", repo_path, temp_git_dir], check=True)
        
        # Switch to benchmarks branch (create if missing)
        subprocess.run(["git", "-C", temp_git_dir, "checkout", "benchmarks"], 
                       stderr=subprocess.DEVNULL)
        
        res = subprocess.run(["git", "-C", temp_git_dir, "rev-parse", "--verify", "benchmarks"],
                             capture_output=True)
        if res.returncode != 0:
            subprocess.run(["git", "-C", temp_git_dir, "checkout", "--orphan", "benchmarks"], check=True)
            subprocess.run(["git", "-C", temp_git_dir, "rm", "-rf", "."], check=True)
            
        dest_assets = os.path.join(temp_git_dir, "benchmarks", "assets")
        os.makedirs(dest_assets, exist_ok=True)
        for img in os.listdir(temp_img_dir):
            shutil.copy(os.path.join(temp_img_dir, img), os.path.join(dest_assets, img))
            
        readme_content = "# Caeneus Benchmark Graphs\n\nThis branch contains automatically generated benchmark charts."
        with open(os.path.join(temp_git_dir, "README.md"), "w") as f:
            f.write(readme_content)
            
        subprocess.run(["git", "-C", temp_git_dir, "add", "."], check=True)
        commit_res = subprocess.run(["git", "-C", temp_git_dir, "commit", "-m", "update benchmark graphs"], 
                                    capture_output=True)
        if b"nothing to commit" in commit_res.stdout or b"nothing to commit" in commit_res.stderr:
            print("No changes in benchmark performance. Skipping upload.")
            return
            
        subprocess.run(["git", "-C", temp_git_dir, "push", "origin", "benchmarks"], check=True)
        print("Successfully uploaded graphs to branch 'benchmarks'.")

def main():
    parser = argparse.ArgumentParser(description="Run benchmarks and generate comparative graphs.")
    parser.add_argument("--dry-run", action="store_true", help="Generate graphs locally in 'benchmarks/assets/' without uploading them.")
    args = parser.parse_args()

    build_project()
    
    if args.dry_run:
        target_dir = "benchmarks/assets"
        os.makedirs(target_dir, exist_ok=True)
        print(f"Dry-run mode: Output graphs will be saved locally to '{target_dir}/'")
    else:
        temp_dir_ctx = tempfile.TemporaryDirectory()
        target_dir = temp_dir_ctx.name

    try:
        # Zig
        try:
            zig_data = run_zig()
            plot_zig(zig_data, os.path.join(target_dir, "zig_benchmark.png"))
        except Exception as e:
            print(f"Error running/plotting Zig: {e}", file=sys.stderr)
            
        # Go
        try:
            go_data = run_go()
            plot_comparison("go", go_data, os.path.join(target_dir, "go_benchmark.png"))
        except Exception as e:
            print(f"Error running/plotting Go: {e}", file=sys.stderr)
            
        # Python
        try:
            py_data = run_python()
            plot_comparison("python", py_data, os.path.join(target_dir, "python_benchmark.png"))
        except Exception as e:
            print(f"Error running/plotting Python: {e}", file=sys.stderr)
            
        # Node
        try:
            node_data = run_node()
            plot_comparison("node", node_data, os.path.join(target_dir, "node_benchmark.png"))
        except Exception as e:
            print(f"Error running/plotting Node: {e}", file=sys.stderr)
            
        if not args.dry_run:
            push_to_benchmarks_branch(target_dir)
            temp_dir_ctx.cleanup()
        else:
            print(f"Completed! All graphs saved locally in '{target_dir}/'")

    except Exception as e:
        print(f"Error during execution: {e}", file=sys.stderr)
        if not args.dry_run:
            temp_dir_ctx.cleanup()

if __name__ == "__main__":
    main()
