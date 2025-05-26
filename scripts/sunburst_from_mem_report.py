import json
import pandas as pd
import plotly.express as px
import argparse
import os.path


def flatten(node, parent_id=None, result=None):
    """
    Recursively flattens the nested memory report into a list of records
    with id, label, parent, and value for sunburst plotting.
    """
    if result is None:
        result = []
    name = node.get('name', '')
    node_id = f"{parent_id}/{name}" if parent_id else name
    result.append({
        'id': node_id,
        'label': name,
        'parent': parent_id or "",
        'value': node.get('size', 0)
    })
    for child in node.get('children', []):
        flatten(child, node_id, result)
    return result


def create_sunburst_from_json(json_path, output_html=None):
    # Load the JSON report
    with open(json_path, 'r') as f:
        report = json.load(f)

    root = report['symbols']
    records = flatten(root)
    df = pd.DataFrame(records)

    # Determine if it's RAM or ROM from filename
    memory_type = "RAM" if "ram" in json_path.lower() else "ROM"

    # Create sunburst chart with children filling full parent
    fig = px.sunburst(
        df,
        ids='id',
        names='label',
        parents='parent',
        values='value',
        title=f'{memory_type} Memory Usage Sunburst',
        branchvalues='total'  # ensure children sum to full parent
    )
    fig.update_layout(margin=dict(t=80, l=0, r=0, b=0))

    if output_html:
        fig.write_html(output_html, include_plotlyjs='cdn', full_html=True)
        print(f"Saved interactive chart to {output_html}")

    fig.show()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate a sunburst chart from memory report. ' \
    'Either ram.json or rom.json, generated with west build -t ram_report/rom_report')
    parser.add_argument('input_file', help='Path to the input JSON memory report file')
    args = parser.parse_args()

    # Generate output filename based on input filename
    base_name = os.path.splitext(args.input_file)[0]
    output_html = f"{base_name}.html"

    create_sunburst_from_json(
        json_path=args.input_file,
        output_html=output_html
    )
