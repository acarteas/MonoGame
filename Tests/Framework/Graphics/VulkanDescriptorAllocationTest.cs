#if VULKAN
using System;
using System.IO;
using System.Linq;
using Microsoft.Xna.Framework;
using Microsoft.Xna.Framework.Graphics;
using NUnit.Framework;

namespace MonoGame.Tests.Graphics
{
    [NonParallelizable]
    [RunOnUiTestFixture]
    internal class VulkanDescriptorAllocationTest : GraphicsDeviceTestFixtureBase
    {
        [Test]
        public void LoadingStockEffectsDoesNotExhaustLinuxMemoryMappings()
        {
            if (!OperatingSystem.IsLinux())
            {
                Assert.Ignore("Linux memory mapping regression.");
            }

            int before = File.ReadLines("/proc/self/maps").Count();
            using var basic = new BasicEffect(gd);
            using var alpha = new AlphaTestEffect(gd);
            using var dual = new DualTextureEffect(gd);
            using var environment = new EnvironmentMapEffect(gd);
            using var skinned = new SkinnedEffect(gd);
            int addedMappings = File.ReadLines("/proc/self/maps").Count() - before;
            Assert.That(addedMappings, Is.LessThan(4096),
                "Loading unused shader variants must leave room for the application's assets and draws.");
        }

        [Test]
        public void ManyTextureBindingsRemainCorrectAcrossFrames()
        {
            const int textureCount = 600;
            var textures = new Texture2D[textureCount];
            var expected = new Color[textureCount];
            using var target = new RenderTarget2D(gd, textureCount, 1, false, SurfaceFormat.Color, DepthFormat.None);
            using var sprites = new SpriteBatch(gd);
            try
            {
                for (int i = 0; i < textureCount; i++)
                {
                    expected[i] = new Color((byte)i, (byte)(i >> 8), (byte)(255 - i % 256), (byte)255);
                    textures[i] = new Texture2D(gd, 1, 1);
                    textures[i].SetData(new[] { expected[i] });
                }

                for (int frame = 0; frame < 5; frame++)
                {
                    gd.SetRenderTarget(target);
                    gd.Clear(Color.Transparent);
                    sprites.Begin(SpriteSortMode.Immediate, BlendState.Opaque, SamplerState.PointClamp);
                    for (int i = 0; i < textureCount; i++)
                    {
                        sprites.Draw(textures[i], new Rectangle(i, 0, 1, 1), Color.White);
                    }
                    sprites.End();
                    gd.SetRenderTarget(null);
                    var actual = new Color[textureCount];
                    target.GetData(actual);
                    Assert.That(actual, Is.EqualTo(expected), "Frame " + frame);
                    gd.Present();
                }
            }
            finally
            {
                gd.SetRenderTarget(null);
                foreach (var texture in textures)
                {
                    texture?.Dispose();
                }
            }
        }
    }
}
#endif
