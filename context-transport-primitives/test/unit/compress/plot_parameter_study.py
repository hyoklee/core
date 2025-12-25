#!/usr/bin/env python3
"""
Compression Parameter Study - Detailed Statistics Report

Generates a comprehensive multi-page report with 5 figures per page for each
distribution showing how parameters affect compression performance.

Each page contains combo charts showing:
1. (Compress Library, Chunk size) vs Compress Time
2. (Compress Library, Chunk size) vs Decompress Time
3. (Compress Library, Chunk size) vs Compression Ratio
4. (Compress Library, Chunk size) vs Compress CPU %
5. (Compress Library, Chunk size) vs Decompress CPU %
"""

import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np
import sys
import os
from pathlib import Path
from matplotlib.backends.backend_pdf import PdfPages

# Set style for better-looking plots
sns.set_style("whitegrid")
plt.rcParams['font.size'] = 9
plt.rcParams['axes.labelsize'] = 10
plt.rcParams['axes.titlesize'] = 11
plt.rcParams['xtick.labelsize'] = 8
plt.rcParams['ytick.labelsize'] = 8
plt.rcParams['legend.fontsize'] = 8

# Color palette for libraries - using distinct colors
LIBRARY_COLORS = {
    # Lossless compressors
    'BZIP2': '#1f77b4',
    'Blosc2': '#ff7f0e',
    'Brotli': '#2ca02c',
    'LZ4': '#d62728',
    'LZO': '#9467bd',
    'Lzma': '#8c564b',
    'Snappy': '#e377c2',
    'Zlib': '#7f7f7f',
    'Zstd': '#bcbd22',
    'LibPressio-BZIP2': '#17becf',
    'LibPressio-BLOSC': '#aec7e8',
    # Lossy compressors
    'LibPressio-ZFP': '#ff9896',
    'LibPressio-BitGrooming': '#98df8a',
}

# Output directory for plots
OUTPUT_DIR = Path(__file__).parent / 'benchmark_plots'
OUTPUT_DIR.mkdir(exist_ok=True)

def load_data(csv_path):
    """Load and preprocess benchmark data."""
    try:
        df = pd.read_csv(csv_path)
        print(f"✓ Loaded {len(df)} benchmark records")

        # Convert chunk size to MB for readability
        df['Chunk Size (MB)'] = df['Chunk Size (bytes)'] / (1024**2)

        # Filter out invalid data
        df = df[df['Compress Time (ms)'] > 0]
        df = df[df['Decompress Time (ms)'] > 0]
        df = df[df['Compression Ratio'] > 0]

        print(f"✓ Processed {len(df)} valid records")
        return df
    except Exception as e:
        print(f"✗ Error loading CSV: {e}")
        sys.exit(1)

def create_simple_bar_chart(ax, df_subset, libraries, metric, ylabel, title, use_log_scale=False):
    """
    Create a simple bar chart for a fixed chunk size (64KB).

    Args:
        ax: Matplotlib axis
        df_subset: DataFrame subset for this distribution
        libraries: List of unique libraries
        metric: Column name for the metric to plot
        ylabel: Y-axis label
        title: Chart title
        use_log_scale: Whether to use log scale for y-axis
    """
    # Number of libraries
    n_libs = len(libraries)
    x_positions = np.arange(n_libs)

    values = []
    for lib in libraries:
        lib_data = df_subset[df_subset['Library'] == lib]
        if len(lib_data) > 0:
            val = lib_data[metric].mean()
            # For log scale, replace 0 or very small values with a minimum threshold
            if use_log_scale and val <= 0:
                val = 0.001
            values.append(val)
        else:
            values.append(0.001 if use_log_scale else 0)

    # Create color list
    colors = [LIBRARY_COLORS.get(lib, '#808080') for lib in libraries]

    # Plot bars
    ax.bar(x_positions, values, color=colors, alpha=0.8, edgecolor='black', linewidth=0.5)

    # Customize chart
    ax.set_xlabel('Compression Library', fontweight='bold')
    ax.set_ylabel(ylabel, fontweight='bold')
    ax.set_title(title, fontweight='bold', pad=10)
    ax.set_xticks(x_positions)
    ax.set_xticklabels(libraries, rotation=45, ha='right')
    ax.grid(True, alpha=0.3, linestyle='--', axis='y')

    if use_log_scale:
        ax.set_yscale('log')

def create_page(df_subset, distribution, libraries):
    """
    Create a page with 5 bar charts for a specific distribution (64KB fixed size).

    Args:
        df_subset: DataFrame subset for this distribution
        distribution: Distribution name
        libraries: List of unique libraries
    """
    fig, axes = plt.subplots(3, 2, figsize=(11, 14))
    fig.suptitle(f'Parameter Study: {distribution}\nChar Data Type, 64KB Chunk Size',
                 fontsize=14, fontweight='bold', y=0.995)

    # 1. Compress Time (log scale)
    create_simple_bar_chart(
        axes[0, 0], df_subset, libraries,
        'Compress Time (ms)', 'Time (ms, log scale)',
        'Compression Time', use_log_scale=True
    )

    # 2. Decompress Time (log scale)
    create_simple_bar_chart(
        axes[0, 1], df_subset, libraries,
        'Decompress Time (ms)', 'Time (ms, log scale)',
        'Decompression Time', use_log_scale=True
    )

    # 3. Compression Ratio (log scale)
    create_simple_bar_chart(
        axes[1, 0], df_subset, libraries,
        'Compression Ratio', 'Ratio (log scale, higher = better)',
        'Compression Ratio', use_log_scale=True
    )

    # 4. Compress CPU %
    create_simple_bar_chart(
        axes[1, 1], df_subset, libraries,
        'Compress CPU %', 'CPU Utilization (%)',
        'Compression CPU Usage'
    )

    # 5. Decompress CPU %
    create_simple_bar_chart(
        axes[2, 0], df_subset, libraries,
        'Decompress CPU %', 'CPU Utilization (%)',
        'Decompression CPU Usage'
    )

    # 6. Summary statistics table
    axes[2, 1].axis('off')

    # Sort by compression ratio descending
    df_sorted = df_subset.sort_values('Compression Ratio', ascending=False)

    table_data = []
    for idx, row in df_sorted.iterrows():
        table_data.append([
            row['Library'],
            f"{row['Compression Ratio']:.2f}x",
            f"{row['Compress Time (ms)']:.1f}",
            f"{row['Compress CPU %']:.0f}%"
        ])

    table = axes[2, 1].table(
        cellText=table_data[:9],  # Top 9 libraries
        colLabels=['Library', 'Ratio', 'Time (ms)', 'CPU%'],
        loc='center',
        cellLoc='center'
    )
    table.auto_set_font_size(False)
    table.set_fontsize(8)
    table.scale(1, 2)

    # Color header
    for i in range(4):
        table[(0, i)].set_facecolor('#4472C4')
        table[(0, i)].set_text_props(weight='bold', color='white')

    # Color best result (first row)
    for i in range(4):
        table[(1, i)].set_facecolor('#E7E6E6')

    axes[2, 1].set_title('Performance Summary (64KB chunks)\nSorted by Compression Ratio',
                        fontweight='bold', pad=10)

    plt.tight_layout(rect=[0, 0, 1, 0.99])
    return fig

def generate_statistics_report(df, output_file):
    """Generate detailed text statistics report."""
    with open(output_file, 'w') as f:
        f.write("=" * 80 + "\n")
        f.write("COMPRESSION PARAMETER STUDY - DETAILED STATISTICS\n")
        f.write("=" * 80 + "\n\n")

        distributions = sorted(df['Distribution'].unique())
        libraries = sorted(df['Library'].unique())

        f.write(f"Total Records: {len(df)}\n")
        f.write(f"Distributions: {len(distributions)}\n")
        f.write(f"Libraries: {len(libraries)}\n")
        f.write(f"Chunk Sizes: {len(df['Chunk Size (MB)'].unique())}\n\n")

        # Group distributions by type
        uniform_dists = sorted([d for d in distributions if d.startswith('uniform_')])
        normal_dists = sorted([d for d in distributions if d.startswith('normal_')])
        other_dists = sorted([d for d in distributions if not d.startswith('uniform_') and not d.startswith('normal_')])

        # Uniform distribution analysis
        if uniform_dists:
            f.write("=" * 80 + "\n")
            f.write("UNIFORM DISTRIBUTION ANALYSIS\n")
            f.write("=" * 80 + "\n\n")
            f.write("Parameter: Max value (controls entropy)\n")
            f.write("  Lower max value = lower entropy = better compression\n\n")

            for lib in libraries:
                f.write(f"{lib}:\n")
                f.write(f"  {'Distribution':<15} {'Max Val':<8} {'Avg Ratio':<12} {'Avg Time (ms)':<15} {'Avg CPU%':<10}\n")
                f.write(f"  {'-'*15} {'-'*8} {'-'*12} {'-'*15} {'-'*10}\n")

                for dist in uniform_dists:
                    subset = df[(df['Library'] == lib) & (df['Distribution'] == dist)]
                    if len(subset) > 0:
                        max_val = dist.split('_')[1]
                        avg_ratio = subset['Compression Ratio'].mean()
                        avg_time = subset['Compress Time (ms)'].mean()
                        avg_cpu = subset['Compress CPU %'].mean()
                        f.write(f"  {dist:<15} {max_val:<8} {avg_ratio:<12.2f} {avg_time:<15.1f} {avg_cpu:<10.0f}\n")
                f.write("\n")

        # Normal distribution analysis
        if normal_dists:
            f.write("=" * 80 + "\n")
            f.write("NORMAL DISTRIBUTION ANALYSIS\n")
            f.write("=" * 80 + "\n\n")
            f.write("Parameter: Standard deviation (controls clustering)\n")
            f.write("  Lower stddev = tighter clustering = better compression\n\n")

            for lib in libraries:
                f.write(f"{lib}:\n")
                f.write(f"  {'Distribution':<15} {'StdDev':<8} {'Avg Ratio':<12} {'Avg Time (ms)':<15} {'Avg CPU%':<10}\n")
                f.write(f"  {'-'*15} {'-'*8} {'-'*12} {'-'*15} {'-'*10}\n")

                # Sort normal distributions: numeric ones first by value, then 'float'
                def sort_key(x):
                    suffix = x.split('_')[1]
                    try:
                        return (0, int(suffix))  # Numeric distributions first
                    except ValueError:
                        return (1, suffix)  # 'float' comes last

                for dist in sorted(normal_dists, key=sort_key):
                    subset = df[(df['Library'] == lib) & (df['Distribution'] == dist)]
                    if len(subset) > 0:
                        stddev = dist.split('_')[1]
                        avg_ratio = subset['Compression Ratio'].mean()
                        avg_time = subset['Compress Time (ms)'].mean()
                        avg_cpu = subset['Compress CPU %'].mean()
                        f.write(f"  {dist:<15} {stddev:<8} {avg_ratio:<12.2f} {avg_time:<15.1f} {avg_cpu:<10.0f}\n")
                f.write("\n")

        # Best compressor analysis
        f.write("=" * 80 + "\n")
        f.write("BEST COMPRESSOR BY DISTRIBUTION (64KB chunks)\n")
        f.write("=" * 80 + "\n\n")

        # Use the chunk size that exists in the data (64KB = 0.0625 MB)
        chunk_size_mb = df['Chunk Size (MB)'].unique()[0]
        df_chunk = df[df['Chunk Size (MB)'] == chunk_size_mb]
        f.write(f"{'Distribution':<20} {'Best Library':<15} {'Ratio':<10} {'Time (ms)':<12}\n")
        f.write(f"{'-'*20} {'-'*15} {'-'*10} {'-'*12}\n")

        for dist in distributions:
            dist_data = df_chunk[df_chunk['Distribution'] == dist]
            if len(dist_data) > 0:
                best = dist_data.loc[dist_data['Compression Ratio'].idxmax()]
                f.write(f"{dist:<20} {best['Library']:<15} {best['Compression Ratio']:<10.2f} {best['Compress Time (ms)']:<12.1f}\n")

        # Parameter effect summary
        f.write("\n")
        f.write("=" * 80 + "\n")
        f.write("KEY FINDINGS\n")
        f.write("=" * 80 + "\n\n")

        if uniform_dists:
            f.write("1. ENTROPY EFFECT (Uniform Distribution):\n")
            # Compare uniform_15 vs uniform_255 for each library
            chunk_size_mb = df['Chunk Size (MB)'].unique()[0]
            for lib in libraries[:3]:  # Top 3
                u15 = df[(df['Library'] == lib) & (df['Distribution'] == 'uniform_15') & (df['Chunk Size (MB)'] == chunk_size_mb)]
                u255 = df[(df['Library'] == lib) & (df['Distribution'] == 'uniform_255') & (df['Chunk Size (MB)'] == chunk_size_mb)]
                if len(u15) > 0 and len(u255) > 0:
                    ratio_15 = u15['Compression Ratio'].iloc[0]
                    ratio_255 = u255['Compression Ratio'].iloc[0]
                    improvement = ((ratio_15 / ratio_255) - 1) * 100
                    f.write(f"   {lib}: uniform_15 compresses {improvement:.0f}% better than uniform_255\n")
            f.write("\n")

        if normal_dists:
            f.write("2. CLUSTERING EFFECT (Normal Distribution):\n")
            # Compare normal_10 vs normal_80 for each library
            chunk_size_mb = df['Chunk Size (MB)'].unique()[0]
            for lib in libraries[:3]:  # Top 3
                n10 = df[(df['Library'] == lib) & (df['Distribution'] == 'normal_10') & (df['Chunk Size (MB)'] == chunk_size_mb)]
                n80 = df[(df['Library'] == lib) & (df['Distribution'] == 'normal_80') & (df['Chunk Size (MB)'] == chunk_size_mb)]
                if len(n10) > 0 and len(n80) > 0:
                    ratio_10 = n10['Compression Ratio'].iloc[0]
                    ratio_80 = n80['Compression Ratio'].iloc[0]
                    improvement = ((ratio_10 / ratio_80) - 1) * 100
                    f.write(f"   {lib}: normal_10 compresses {improvement:.0f}% better than normal_80\n")
            f.write("\n")

        f.write("3. IMPLICATIONS FOR REAL-WORLD DATA:\n")
        f.write("   - Lower entropy data (limited value range) compresses much better\n")
        f.write("   - Clustered data (low variance) compresses much better\n")
        f.write("   - Random noise (uniform_255, normal_80) is nearly incompressible\n")
        f.write("   - Many scientific datasets have structure and compress well\n\n")

def generate_best_compressor_table(df, output_file):
    """Generate table showing best compressor for each distribution (64KB fixed)."""
    with open(output_file, 'w') as f:
        f.write("BEST COMPRESSOR BY DISTRIBUTION (64KB Chunk Size)\n")
        f.write("=" * 80 + "\n\n")

        distributions = sorted(df['Distribution'].unique())

        f.write(f"{'Distribution':<25} {'Best Library':<15} {'Ratio':<10} {'Time (ms)':<12} {'CPU %':<8}\n")
        f.write("-" * 80 + "\n")

        for dist in distributions:
            subset = df[df['Distribution'] == dist]
            if len(subset) > 0:
                best = subset.loc[subset['Compression Ratio'].idxmax()]
                f.write(f"{dist:<25} {best['Library']:<15} {best['Compression Ratio']:<10.2f} "
                       f"{best['Compress Time (ms)']:<12.1f} {best['Compress CPU %']:<8.0f}\n")

def main():
    """Main function to generate parameter study report."""
    print("=" * 80)
    print("COMPRESSION PARAMETER STUDY VISUALIZATION")
    print("=" * 80)
    print()

    # Find CSV files (both lossless and lossy)
    build_dir = Path(__file__).parent.parent.parent.parent.parent / 'build'
    lossless_csv_path = build_dir / 'compression_parameter_study_results.csv'
    lossy_csv_path = build_dir / 'compression_lossy_parameter_study_results.csv'

    # Check if at least one file exists
    if not lossless_csv_path.exists() and not lossy_csv_path.exists():
        print(f"✗ Error: No results files found")
        print(f"  Expected lossless: {lossless_csv_path}")
        print(f"  Expected lossy: {lossy_csv_path}")
        print("\nPlease run the parameter study benchmarks first:")
        print("  cd /workspace/build")
        print("  ./bin/benchmark_compress_parameter_study_exec")
        print("  ./bin/benchmark_compress_lossy_parameter_study_exec")
        sys.exit(1)

    # Load and combine data
    dfs = []
    if lossless_csv_path.exists():
        print(f"Loading lossless data from: {lossless_csv_path}")
        df_lossless = load_data(lossless_csv_path)
        dfs.append(df_lossless)
        print(f"  ✓ Loaded {len(df_lossless)} lossless compression results")

    if lossy_csv_path.exists():
        print(f"Loading lossy data from: {lossy_csv_path}")
        df_lossy = load_data(lossy_csv_path)
        dfs.append(df_lossy)
        print(f"  ✓ Loaded {len(df_lossy)} lossy compression results")

    # Combine dataframes
    df = pd.concat(dfs, ignore_index=True)
    print(f"✓ Combined total: {len(df)} compression results")
    print()

    # Get unique values
    distributions = sorted(df['Distribution'].unique())
    libraries = sorted(df['Library'].unique())
    chunk_sizes = sorted(df['Chunk Size (MB)'].unique())

    print(f"✓ Found {len(distributions)} distributions")
    print(f"✓ Found {len(libraries)} libraries")
    print(f"✓ Found {len(chunk_sizes)} chunk sizes")
    print()

    # Generate PDF report
    pdf_path = OUTPUT_DIR / 'parameter_study_full_report.pdf'
    print(f"Generating PDF report: {pdf_path}")

    with PdfPages(pdf_path) as pdf:
        for i, dist in enumerate(distributions, 1):
            print(f"  Page {i}/{len(distributions)}: {dist}")
            df_dist = df[df['Distribution'] == dist]

            if len(df_dist) > 0:
                fig = create_page(df_dist, dist, libraries)
                pdf.savefig(fig, bbox_inches='tight')
                plt.close(fig)

    print(f"✓ PDF report saved: {pdf_path.name} ({len(distributions)} pages)")
    print()

    # Generate statistics report
    stats_path = OUTPUT_DIR / 'parameter_study_statistics.txt'
    print(f"Generating statistics report: {stats_path}")
    generate_statistics_report(df, stats_path)
    print(f"✓ Statistics saved: {stats_path.name}")
    print()

    # Generate best compressor table
    table_path = OUTPUT_DIR / 'parameter_study_best_compressor.txt'
    print(f"Generating best compressor table: {table_path}")
    generate_best_compressor_table(df, table_path)
    print(f"✓ Table saved: {table_path.name}")
    print()

    print("=" * 80)
    print("✓ REPORT GENERATION COMPLETE")
    print("=" * 80)
    print()
    print("Generated files:")
    print(f"  • {pdf_path.name} - {len(distributions)}-page detailed report")
    print(f"  • {stats_path.name} - Detailed statistics and analysis")
    print(f"  • {table_path.name} - Best compressor lookup table")
    print()

if __name__ == '__main__':
    main()
