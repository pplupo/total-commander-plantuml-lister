import os
import re
import requests
import xml.etree.ElementTree as ET
import json

RESOURCES_DIR = os.path.join(os.path.dirname(__file__), '..', 'resources')
INI_FILE = os.path.join(os.path.dirname(__file__), '..', 'plantumlwebview.ini')
GITHUB_API_URL = 'https://api.github.com/repos/plantuml/plantuml/releases'
SOURCEFORGE_RSS_URL = 'https://sourceforge.net/projects/plantuml.mirror/rss'

def get_latest_mit_jar_from_github():
    """
    Fetches the latest MIT licensed plantuml jar from the GitHub releases API.
    """
    print("Trying to fetch from GitHub releases...")
    response = requests.get(GITHUB_API_URL)
    response.raise_for_status()
    releases = response.json()

    for release in releases:
        if not release['prerelease'] and not release['draft']:
            for asset in release['assets']:
                if 'mit' in asset['name'] and asset['name'].endswith('.jar') and 'sources' not in asset['name'] and 'javadoc' not in asset['name']:
                    return asset['browser_download_url'], asset['name']

    return None, None

def get_latest_mit_jar_from_sourceforge():
    """
    Fetches the latest MIT licensed plantuml jar from the SourceForge RSS feed.
    """
    print("Trying to fetch from SourceForge RSS feed...")
    response = requests.get(SOURCEFORGE_RSS_URL)
    response.raise_for_status()
    root = ET.fromstring(response.content)

    latest_version = None
    latest_url = None
    latest_filename = None

    for item in root.findall('.//item'):
        title = item.find('title').text
        if 'plantuml-mit-' in title and title.endswith('.jar') and 'sources' not in title and 'javadoc' not in title:
            # Extract version from filename, e.g. plantuml-mit-1.2025.7.jar
            match = re.search(r'plantuml-mit-(.*?).jar', title)
            if match:
                version_str = match.group(1)
                version_tuple = tuple(map(int, version_str.split('.')))

                if latest_version is None or version_tuple > latest_version:
                    latest_version = version_tuple
                    latest_url = item.find('link').text
                    latest_filename = os.path.basename(title)

    return latest_url, latest_filename

def update_ini_file(new_jar_filename):
    """
    Updates the plantumlwebview.ini file with the new jar filename.
    """
    with open(INI_FILE, 'r') as f:
        lines = f.readlines()

    with open(INI_FILE, 'w') as f:
        for line in lines:
            if line.strip().startswith('jar='):
                f.write(f'jar={new_jar_filename}\n')
            else:
                f.write(line)

def main():
    """
    Main function to update the plantuml jar.
    """
    url, filename = get_latest_mit_jar_from_github()

    if not url or not filename:
        print("Could not find the latest MIT licensed plantuml jar on GitHub releases.")
        url, filename = get_latest_mit_jar_from_sourceforge()

    if not url or not filename:
        print("Could not find the latest MIT licensed plantuml jar on both GitHub and SourceForge.")
        return

    print(f"Found latest version: {filename}")

    # Check if the latest version is already downloaded
    new_jar_path = os.path.join(RESOURCES_DIR, filename)
    if os.path.exists(new_jar_path):
        print("Latest version already downloaded.")
    else:
        # Download the new jar
        print(f"Downloading {filename} from {url}...")
        response = requests.get(url, allow_redirects=True)
        response.raise_for_status()

        # Save the new jar
        with open(new_jar_path, 'wb') as f:
            f.write(response.content)

        print(f"Saved new jar to {new_jar_path}")

    # Update the ini file
    print(f"Updating {INI_FILE}...")
    update_ini_file(filename)

    # Remove old jar files
    for item in os.listdir(RESOURCES_DIR):
        if item.startswith('plantuml-mit-') and item.endswith('.jar') and item != filename:
            print(f"Removing old jar: {item}")
            os.remove(os.path.join(RESOURCES_DIR, item))

    print("Done.")

if __name__ == '__main__':
    main()
