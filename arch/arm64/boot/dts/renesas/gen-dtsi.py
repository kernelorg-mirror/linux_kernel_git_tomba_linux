#!/usr/bin/env python3

import argparse
import os
import jinja2
import yaml

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate GMSL dtsi')
    parser.add_argument('-o', '--output', help='dtsi output path')
    parser.add_argument('config', help='YAML config file')

    args = parser.parse_args()

    config_dir = os.path.dirname(args.config)

    with open(args.config, encoding='utf-8') as file:
        config = yaml.safe_load(file)

    template_file = config_dir + '/' + config['template']
    template_dir = os.path.dirname(template_file)
    template_file = os.path.basename(template_file)

    environment = jinja2.Environment(loader=jinja2.FileSystemLoader(template_dir),
                                     trim_blocks=True)
    template = environment.get_template(template_file)

    s = template.render(config)

    if args.output:
        with open(args.output, mode='w', encoding='utf-8') as file:
            file.write(s)
    else:
        print(s)
