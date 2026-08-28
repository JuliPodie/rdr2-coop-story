import type { Metadata } from 'next';
import './globals.css';

const title = 'RDR2 Coop Story — Protocol 32 Tester Build';
const description = 'An experimental, non-commercial private RDR2 Story Mode co-op tester build by Lifeely.';
const siteUrl = process.env.NEXT_PUBLIC_SITE_URL;
const socialImage = siteUrl
  ? new URL('og.png', siteUrl.endsWith('/') ? siteUrl : `${siteUrl}/`).toString()
  : undefined;

export const metadata: Metadata = {
  title,
  description,
  openGraph: {
    title,
    description,
    type: 'website',
    images: socialImage ? [{ url: socialImage, width: 1200, height: 630, alt: 'RDR2 Coop Story — Player Replication Foundation' }] : [],
  },
  twitter: {
    card: 'summary_large_image',
    title,
    description,
    images: socialImage ? [socialImage] : [],
  },
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
