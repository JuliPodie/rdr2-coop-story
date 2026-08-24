import type { NextConfig } from 'next';

const repositoryName = process.env.GITHUB_REPOSITORY?.split('/')[1] ?? '';
const isUserOrOrganizationSite = repositoryName.endsWith('.github.io');
const basePath = process.env.GITHUB_ACTIONS && repositoryName && !isUserOrOrganizationSite
  ? `/${repositoryName}`
  : '';

const nextConfig: NextConfig = {
  output: 'export',
  trailingSlash: true,
  basePath,
  assetPrefix: basePath,
};

export default nextConfig;
